add_executable(rgbt_common_tests common/version_test.cpp)

target_link_libraries(rgbt_common_tests PRIVATE rgbt_common GTest::gtest_main)

# 测试目标使用 rgbt_set_test_warnings 而不是项目级告警策略：
# GTest 的 TEST 宏生成的静态函数会被 -Wunused-function 误报。
rgbt_set_test_warnings(rgbt_common_tests)
rgbt_apply_sanitizer(rgbt_common_tests)
rgbt_apply_sanitizer(rgbt_common)

include(GoogleTest)
gtest_discover_tests(rgbt_common_tests)
