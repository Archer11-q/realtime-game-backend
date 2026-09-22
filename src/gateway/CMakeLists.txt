# gateway 服务：TASK-005 登录切片 + TASK-006 玩家档案走数据库。
#
# 依赖 vcpkg 提供的 brpc、hiredis 与 libmariadb，因此本目录只在启用 vcpkg
# 工具链时加入（见 src/CMakeLists.txt），保证默认构建与 CI 不受影响。
#
# 本目录还负责生成 api/proto 下的 Protobuf 代码：契约是接口真相来源，
# 生成物放在构建目录，不进入版本库。

# ---------------------------------------------------------------------------
# Protobuf 代码生成
# ---------------------------------------------------------------------------

find_package(Protobuf REQUIRED)
# vcpkg 的 brpc 以 unofficial-brpc 为包名导出，目标名是 unofficial::brpc::brpc。
# 必须先 find_package，否则链接时找不到该 IMPORTED 目标。
find_package(unofficial-brpc CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
# MySQL 客户端。TASK-006 起用于读取玩家档案（Phase 1 临时安排，见 ADR-0002）。
find_package(unofficial-libmariadb CONFIG REQUIRED)
# 密码摘要使用 OpenSSL 的 EVP 接口。openssl 已由 brpc 与 libmariadb 引入，
# 这里显式声明以便直接使用其头文件，不新增组件。
find_package(OpenSSL REQUIRED)

set(RGBT_PROTO_DIR "${PROJECT_SOURCE_DIR}/api/proto")
set(RGBT_PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto-gen")

file(MAKE_DIRECTORY "${RGBT_PROTO_OUT_DIR}")

add_custom_command(
  OUTPUT "${RGBT_PROTO_OUT_DIR}/gateway.pb.cc" "${RGBT_PROTO_OUT_DIR}/gateway.pb.h"
  COMMAND protobuf::protoc
  ARGS
    --cpp_out "${RGBT_PROTO_OUT_DIR}"
    --proto_path "${RGBT_PROTO_DIR}"
    "${RGBT_PROTO_DIR}/gateway.proto"
  DEPENDS "${RGBT_PROTO_DIR}/gateway.proto"
  COMMENT "生成 gateway.proto 的 C++ 代码"
  VERBATIM
)

add_library(rgbt_gateway_proto STATIC "${RGBT_PROTO_OUT_DIR}/gateway.pb.cc")
target_include_directories(rgbt_gateway_proto PUBLIC "${RGBT_PROTO_OUT_DIR}")
target_link_libraries(rgbt_gateway_proto PUBLIC protobuf::libprotobuf)

# 生成代码属于第三方产出，不使用 -Werror 以免因 protoc 版本差异导致构建失败。
if(NOT MSVC)
  target_compile_options(rgbt_gateway_proto PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# 服务实现
# ---------------------------------------------------------------------------

add_library(
  rgbt_gateway_lib STATIC
  brpc_match_client.cpp
  database_player_directory.cpp
  error.cpp
  gateway_service.cpp
  in_memory_player_directory.cpp
  mysql_connection.cpp
  mysql_player_reader.cpp
  password_hash.cpp
  redis_session_store.cpp
  test_credentials.cpp
)

target_include_directories(rgbt_gateway_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  rgbt_gateway_lib
  PUBLIC
    rgbt_common
    rgbt_gateway_proto
  PRIVATE
    # Gateway 调用 Match Service（TASK-007），需要 match 的生成代码与服务桩。
    rgbt_match_proto
    unofficial::brpc::brpc
    hiredis::hiredis
    unofficial::libmariadb
    OpenSSL::Crypto
)

rgbt_set_project_warnings(rgbt_gateway_lib)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------

add_executable(rgbt_gateway gateway_main.cpp)
target_link_libraries(rgbt_gateway PRIVATE rgbt_gateway_lib unofficial::brpc::brpc)

set_target_properties(
  rgbt_gateway
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 本目录不定义测试目标，也不判断 GoogleTest 是否可用：测试放在
# tests/unit/gateway/，由 tests 目录自行解析测试依赖。这样服务目录只负责
# 编译服务本身。
