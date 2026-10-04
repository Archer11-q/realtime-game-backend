# gateway 的单元测试。
#
# 放在 tests/ 下而不是 src/gateway/ 内：测试属于验证资产，集中在一处便于查找，
# 也让 src/gateway 只负责编译服务本身。
#
# 头文件路径来自 rgbt_gateway_lib 的 PUBLIC include 声明，因此这里不需要额外
# 指定 include 目录。

add_executable(
  rgbt_gateway_tests
  gateway_service_test.cpp
  player_directory_test.cpp
  stream_hub_test.cpp
)

target_link_libraries(rgbt_gateway_tests PRIVATE rgbt_gateway_lib GTest::gtest_main)

# TASK-021：测试直接构造 `brpc::Controller` 来驱动 HTTP 路径（`request_done` 的
# `op=` 由请求 URI 推导，没有真实的 Controller 就测不到）。
#
# 这里必须显式链接 brpc，而不能只靠 `rgbt_gateway_lib` 传递：库把它作为 PRIVATE
# 依赖，因此 brpc 的**接口编译定义**（vcpkg 的 glog 要求 `GLOG_USE_GLOG_EXPORT`
# 与 `GLOG_USE_GFLAGS`）不会传到测试目标。缺了它们的表现是连 brpc 头文件都编不过
# ——报错是 `<glog/logging.h> was not included correctly`，看不出与 brpc 有关。
find_package(unofficial-brpc CONFIG REQUIRED)
target_link_libraries(rgbt_gateway_tests PRIVATE unofficial::brpc::brpc)

# 测试目标使用 rgbt_set_test_warnings 而不是项目级告警策略：
# GTest 的 TEST_F 宏生成的静态函数会被 -Wunused-function 误报。
rgbt_set_test_warnings(rgbt_gateway_tests)
rgbt_apply_sanitizer(rgbt_gateway_tests)
rgbt_apply_sanitizer(rgbt_gateway_lib)

include(GoogleTest)
gtest_discover_tests(rgbt_gateway_tests)
