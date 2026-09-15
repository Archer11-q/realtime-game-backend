# 测试目录。
#
# 只把 GoogleTest 链接到测试目标，避免测试依赖泄漏到产品目标。

set(RGBT_TESTS_AVAILABLE ON)

if(RGBT_USE_VCPKG_TOOLCHAIN)
  # vcpkg 模式下优先使用包管理器提供的 GTest；没有就跳过测试并说明原因，
  # 而不是中断配置，因为 brpc-debug 预设的目的是验证 brpc 可用性。
  if(DEFINED FETCHCONTENT_SOURCE_DIR_GOOGLETEST)
    message(STATUS "googletest 由 FetchContent 提供，测试可用")
    set(GTest_FOUND TRUE)
  else()
    find_package(GTest CONFIG QUIET)
  endif()
  if(NOT GTest_FOUND)
    set(RGBT_TESTS_AVAILABLE OFF)
    message(
      STATUS
        "vcpkg 中未安装 gtest，跳过单元测试。"
        " 需要测试时执行: vcpkg install gtest"
    )
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

if(RGBT_TESTS_AVAILABLE)
  add_subdirectory(unit)
endif()
