# gateway 的单元测试。
#
# 放在 tests/ 下而不是 src/gateway/ 内：测试属于验证资产，集中在一处便于查找，
# 也让 src/gateway 只负责编译服务本身。
#
# 头文件路径来自 rgbt_gateway_lib 的 PUBLIC include 声明，因此这里不需要额外
# 指定 include 目录。

add_executable(rgbt_gateway_tests gateway_service_test.cpp)

target_link_libraries(rgbt_gateway_tests PRIVATE rgbt_gateway_lib GTest::gtest_main)

# 测试目标使用 rgbt_set_test_warnings 而不是项目级告警策略：
# GTest 的 TEST_F 宏生成的静态函数会被 -Wunused-function 误报。
rgbt_set_test_warnings(rgbt_gateway_tests)
rgbt_apply_sanitizer(rgbt_gateway_tests)
rgbt_apply_sanitizer(rgbt_gateway_lib)

include(GoogleTest)
gtest_discover_tests(rgbt_gateway_tests)
