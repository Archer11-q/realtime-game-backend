# Realtime Game Backend - 顶层构建入口
#
# 本文件只负责构建基线：工程声明、编译标准、警告策略、测试开关和公共子目录。
# 业务服务只有 Gateway / Match / Room-Battle 三个，在各自任务中按需加入。
# 不要为 Player/State 或 Settlement 添加子目录：两者已列入非目标，见
# docs/adr/0003-scope-reduction.md。

cmake_minimum_required(VERSION 3.25)

project(
  realtime_game_backend
  VERSION 0.1.0
  DESCRIPTION "面向小型实时对战产品的游戏服务端参考实现"
  LANGUAGES CXX
)

# 默认使用 Debug，保证单配置生成器下构建类型明确，不依赖隐式默认值。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "构建类型" FORCE
  )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 关闭 GNU 扩展，避免使用 gnu++ 特有行为导致 CI 与本地不一致。
set(CMAKE_CXX_EXTENSIONS OFF)
# 生成 compile_commands.json，供 clang-tidy 和 CLion 使用。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RGBT_WARNINGS_AS_ERRORS "把编译警告视为错误" ON)
option(RGBT_ENABLE_SANITIZER "启用 AddressSanitizer" OFF)
option(RGBT_ENABLE_TSAN "启用 ThreadSanitizer" OFF)
option(RGBT_BUILD_TESTS "构建单元测试" ON)

# ASan 与 TSan 不能同时启用：两者都要接管内存访问路径，同时打开会在链接期
# 或运行期产生难以解释的失败。这里直接报错，而不是让使用者自己猜。
if(RGBT_ENABLE_SANITIZER AND RGBT_ENABLE_TSAN)
  message(FATAL_ERROR "RGBT_ENABLE_SANITIZER 与 RGBT_ENABLE_TSAN 不能同时开启（ASan 与 TSan 互斥）")
endif()

# GoogleTest 获取方式开关：
#   OFF（默认）使用 FetchContent 下载并构建，不需要额外系统配置；
#   ON        改用 find_package，指向 vcpkg 等包管理器。
# 引入 vcpkg 时只需打开此开关并配置 toolchain，测试代码和目标名无需改动。
option(RGBT_USE_VCPKG_TOOLCHAIN "使用包管理器提供的依赖，而不是 FetchContent" OFF)

# vcpkg 工具链位置。仅在需要 brpc 等 vcpkg 依赖时使用（brpc-debug 预设）。
#
# 注意：这里刻意不默认取 $ENV{VCPKG_ROOT}。非交互式 shell（脚本、CI、
# cmake 被上层工具调用时）不会加载 .profile/.bashrc，VCPKG_ROOT 会是空字符串；
# 而 vcpkg 的 toolchain 会把 VCPKG_ROOT 写入缓存，一旦第一次配置写入了错误值，
# 后续重新配置会因为“环境变量与缓存不一致”而直接中止。
# 因此默认值用本机文档记录的固定路径，需要时用 -DRGBT_VCPKG_TOOLCHAIN_FILE 覆盖。
set(RGBT_VCPKG_TOOLCHAIN_FILE
    "$ENV{HOME}/tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE FILEPATH "vcpkg toolchain 文件路径"
)

# 让 debug / release / asan 三个预设共享同一份依赖下载与构建产物。
# 默认行为是每个预设各自下载一次（三个预设即三次网络下载），既慢又放大了
# 网络抖动导致的配置失败概率。共享后只下载一次。
# 该目录位于任何预设的 binaryDir 之外，因此不会被某个预设的清理动作删除。
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/build/_deps")

function(rgbt_set_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(RGBT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
    )
    if(RGBT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(rgbt_set_test_warnings target)
  # 测试代码的告警策略与产品代码不同。
  #
  # 原因：GTest 的 TEST / TEST_F 宏展开出的静态函数只通过框架内部注册表引用，
  # GCC 在 -Wunused-function 下会误报为「未被使用」。这是框架的固有形态，
  # 不是代码缺陷，因此测试目标保留基础告警，但不把警告视为错误。
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-function
    )
  endif()
endfunction()

function(rgbt_apply_sanitizer target)
  set(flags "")
  if(RGBT_ENABLE_SANITIZER)
    list(APPEND flags -fsanitize=address)
  endif()
  if(RGBT_ENABLE_TSAN)
    list(APPEND flags -fsanitize=thread)
    # 关掉 -Wtsan：它会为"TSan 无法正确插桩 atomic_thread_fence"报一条告警，
    # 而该构造出现在 brpc/protobuf 与 libstdc++ 的头文件里，不在本项目代码中
    # （`grep atomic_thread_fence` 在本仓库为空）。开着 -Werror 会因此直接编译失败。
    # 这条告警说明的是"该处 TSan 覆盖不到"，不是代码缺陷，因此在 TSan 构建里屏蔽，
    # 其余告警仍然照常按 -Werror 处理。
    list(APPEND flags -Wno-tsan)
  endif()
  if(NOT flags)
    return()
  endif()
  if(MSVC)
    message(WARNING "MSVC 暂不支持本项目的 Sanitizer 预设，已跳过 ${target}")
    return()
  endif()
  # TSan 要求带帧指针以便给出可读的栈，ASan 同理，因此两者都加。
  target_compile_options(${target} PRIVATE ${flags} -fno-omit-frame-pointer -g)
  target_link_options(${target} PRIVATE ${flags})
endfunction()

# 公共 CMake 函数：为 api/proto 下的契约生成 C++ 代码。
# TASK-008 引入第三个 proto（room.proto）时从三个服务目录中抽取出来。
include(cmake/generate_service_proto.cmake)

# 子目录顺序：先 src 再 tests。
#
# 服务目录只负责编译自己，不判断测试依赖是否可用——那是 tests 的职责。
# tests/CMakeLists.txt 自行解析 GoogleTest 并决定是否加入测试目标。
add_subdirectory(src)

include(CTest)

if(BUILD_TESTING AND RGBT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
