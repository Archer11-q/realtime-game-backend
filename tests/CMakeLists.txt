# 测试目录。
#
# 本目录自行解析 GoogleTest 并决定是否加入测试目标，服务目录不需要知道
# 测试依赖是否可用。
#
# 判定结果写入缓存变量 RGBT_TESTS_AVAILABLE，因为子目录之间不共享普通变量
# 作用域，而 tests/unit/gateway/CMakeLists.txt 需要读取该结果。使用 FORCE
# 保证每次配置都按当前环境重新判定，不会被上次的缓存值掩盖。

set(RGBT_TESTS_AVAILABLE
    ON
    CACHE INTERNAL "GoogleTest 是否可用"
    FORCE
)

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

if(RGBT_TESTS_AVAILABLE)
  add_subdirectory(unit)
endif()
