# Realtime Game Backend - 顶层构建入口
#
# 本文件只负责构建基线：工程声明、编译标准、警告策略、测试开关和公共子目录。
# 业务服务（Gateway/Match/Room/Player/Settlement）在各自任务中按需加入。

cmake_minimum_required(VERSION 3.25)

project(
  realtime_game_backend
  VERSION 0.1.0
  DESCRIPTION "面向小型实时对战产品的分布式游戏服务端参考实现"
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
option(RGBT_BUILD_TESTS "构建单元测试" ON)

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
  if(NOT RGBT_ENABLE_SANITIZER)
    return()
  endif()
  if(MSVC)
    message(WARNING "MSVC 暂不支持本项目的 Sanitizer 预设，已跳过 ${target}")
    return()
  endif()
  target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(${target} PRIVATE -fsanitize=address)
endfunction()

# 测试框架必须在 src 之前准备完毕。
#
# 原因：src/gateway 需要根据「GTest 是否可用」决定是否加入自己的测试目标，
# 而 CMake 按 add_subdirectory 的顺序处理，且子目录之间不共享普通变量作用域。
# 因此在顶层统一解析好依赖并写入缓存变量，再处理 src 与 tests。
include(CTest)

set(RGBT_TESTS_AVAILABLE
    ON
    CACHE INTERNAL "GoogleTest 是否可用"
    FORCE
)

if(BUILD_TESTING AND RGBT_BUILD_TESTS)
  if(RGBT_USE_VCPKG_TOOLCHAIN)
    # vcpkg 模式下优先使用包管理器提供的 GTest；没有就跳过测试并说明原因，
    # 而不是中断配置，因为 brpc 预设的主要目的是验证服务可用性。
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
      set(RGBT_TESTS_AVAILABLE
          OFF
          CACHE INTERNAL "GoogleTest 是否可用"
          FORCE
      )
      message(
        STATUS
          "vcpkg 中未安装 gtest，跳过单元测试。"
          " 需要测试时执行: vcpkg install gtest"
      )
    else()
      message(STATUS "使用 vcpkg 提供的 GoogleTest，单元测试已启用")
    endif()
  else()
    include(FetchContent)
    # 固定到 tag 以保证可复现；升级依赖需要单独提交并记录原因。
    # SYSTEM 让第三方头文件的警告不污染本项目的 -Werror。
    FetchContent_Declare(
      googletest
      SYSTEM
      URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    )
    # Windows 上避免 googletest 覆盖父工程的运行时库设置。
    set(gtest_force_shared_crt
        ON
        CACHE BOOL "" FORCE
    )
    FetchContent_MakeAvailable(googletest)
  endif()
else()
  set(RGBT_TESTS_AVAILABLE
      OFF
      CACHE INTERNAL "GoogleTest 是否可用"
      FORCE
  )
endif()

add_subdirectory(src)

if(RGBT_TESTS_AVAILABLE)
  enable_testing()
  add_subdirectory(tests)
endif()
