# 测试目录。
#
# 只把 GoogleTest 链接到测试目标，避免测试依赖泄漏到产品目标。

if(RGBT_USE_VCPKG_TOOLCHAIN)
  find_package(GTest CONFIG REQUIRED)
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

add_subdirectory(unit)
