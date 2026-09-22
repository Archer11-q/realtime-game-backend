# match 的单元测试。
#
# 放在 tests/ 下而不是 src/match/ 内：测试属于验证资产，集中在一处便于查找，
# 也让 src/match 只负责编译服务本身。
#
# 这些用例只链接 rgbt_match_lib（其中已包含不依赖 brpc 服务器逻辑的 MatchQueue），
# 因此不启动任何网络服务。

add_executable(rgbt_match_tests match_queue_test.cpp)

target_link_libraries(rgbt_match_tests PRIVATE rgbt_match_lib GTest::gtest_main)

# 测试目标使用 rgbt_set_test_warnings 而不是项目级告警策略：
# GTest 的 TEST_F 宏生成的静态函数会被 -Wunused-function 误报。
rgbt_set_test_warnings(rgbt_match_tests)
rgbt_apply_sanitizer(rgbt_match_tests)
rgbt_apply_sanitizer(rgbt_match_lib)

include(GoogleTest)
gtest_discover_tests(rgbt_match_tests)
