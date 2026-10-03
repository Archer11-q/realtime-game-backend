# gateway 服务：TASK-005 登录切片 + TASK-006 玩家档案走数据库 + TASK-007 匹配
# + TASK-008 房间与对局结果 + TASK-009 SSE 推送。
#
# 依赖 vcpkg 提供的 brpc、hiredis 与 libmariadb（后者经 rgbt_common_mysql 传递），
# 因此本目录只在启用 vcpkg 工具链时加入（见 src/CMakeLists.txt），
# 保证默认构建与 CI 不受影响。
#
# Protobuf 代码生成走公共函数 rgbt_add_service_proto（定义在
# cmake/generate_service_proto.cmake，顶层 CMakeLists.txt 已 include）。

# ---------------------------------------------------------------------------
# 依赖
# ---------------------------------------------------------------------------

find_package(Protobuf REQUIRED)
# vcpkg 的 brpc 以 unofficial-brpc 为包名导出，目标名是 unofficial::brpc::brpc。
# 必须先 find_package，否则链接时找不到该 IMPORTED 目标。
find_package(unofficial-brpc CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
# 密码摘要使用 OpenSSL 的 EVP 接口。openssl 已由 brpc 与 libmariadb 引入，
# 这里显式声明以便直接使用其头文件，不新增组件。
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# Protobuf 代码生成
# ---------------------------------------------------------------------------

rgbt_add_service_proto(rgbt_gateway_proto gateway)

# ---------------------------------------------------------------------------
# 服务实现
# ---------------------------------------------------------------------------

add_library(
  rgbt_gateway_lib STATIC
  brpc_match_client.cpp
  brpc_room_client.cpp
  database_player_directory.cpp
  error.cpp
  gateway_service.cpp
  in_memory_player_directory.cpp
  mysql_player_reader.cpp
  password_hash.cpp
  redis_session_store.cpp
  stream_hub.cpp
  test_credentials.cpp
)

target_include_directories(rgbt_gateway_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  rgbt_gateway_lib
  PUBLIC
    rgbt_common
    # MySQL 连接辅助（原在本目录，TASK-008 起被 Room 与 Gateway 共用，
    # 按放置规则移入 include/common/）。
    rgbt_common_mysql
    rgbt_gateway_proto
  PRIVATE
    # Gateway 调用 Match Service（TASK-007）与 Room/Battle Service（TASK-008），
    # 需要两者的生成代码与服务桩。
    rgbt_match_proto
    rgbt_room_proto
    unofficial::brpc::brpc
    hiredis::hiredis
    OpenSSL::Crypto
)

rgbt_set_project_warnings(rgbt_gateway_lib)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------

add_executable(rgbt_gateway gateway_main.cpp)
target_link_libraries(
  rgbt_gateway
  PRIVATE
    rgbt_gateway_lib
    # `/metrics` 端点（TASK-019），理由见 src/match/CMakeLists.txt。
    rgbt_common_metrics_service
    unofficial::brpc::brpc
)

# 服务可执行文件也要过 sanitizer，理由见 src/room/CMakeLists.txt 里的说明：
# 只给 *_lib 加 sanitizer 会让服务二进制链接失败，ASan 实际覆盖不到服务进程。
rgbt_apply_sanitizer(rgbt_gateway)

set_target_properties(
  rgbt_gateway
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 本目录不定义测试目标，也不判断 GoogleTest 是否可用：测试放在
# tests/unit/gateway/，由 tests 目录自行解析测试依赖。这样服务目录只负责
# 编译服务本身。
