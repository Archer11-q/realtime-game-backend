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

add_subdirectory(src)

# 测试骨架：使用标准 CTest 模块提供的 BUILD_TESTING 开关。
include(CTest)
if(BUILD_TESTING AND RGBT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
