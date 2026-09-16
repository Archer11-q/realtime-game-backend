# gateway 服务：TASK-005 登录切片。
#
# 依赖 vcpkg 提供的 brpc 与 hiredis，因此本目录只在启用 vcpkg 工具链时加入
# （见 src/CMakeLists.txt），保证默认构建与 CI 不受影响。
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
  error.cpp
  gateway_service.cpp
  player_directory.cpp
  redis_session_store.cpp
)

target_include_directories(rgbt_gateway_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  rgbt_gateway_lib
  PUBLIC
    rgbt_common
    rgbt_gateway_proto
  PRIVATE
    unofficial::brpc::brpc
    hiredis::hiredis
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

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------

if(RGBT_BUILD_TESTS AND RGBT_TESTS_AVAILABLE)
  add_executable(rgbt_gateway_tests unit/gateway_service_test.cpp)
  target_link_libraries(rgbt_gateway_tests PRIVATE rgbt_gateway_lib GTest::gtest_main)
  target_include_directories(rgbt_gateway_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
  rgbt_set_test_warnings(rgbt_gateway_tests)
  rgbt_apply_sanitizer(rgbt_gateway_tests)
  rgbt_apply_sanitizer(rgbt_gateway_lib)
  include(GoogleTest)
  gtest_discover_tests(rgbt_gateway_tests)
endif()
