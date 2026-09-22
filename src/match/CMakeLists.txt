# match 服务：TASK-007 匹配队列与配对。
#
# 依赖 vcpkg 提供的 brpc，因此本目录只在启用 vcpkg 工具链时加入
# （见 src/CMakeLists.txt），保证默认构建与 CI 不受影响。
#
# 本目录同时生成 api/proto/match.proto 的 Protobuf 代码。契约放在 api/proto/
# 是接口真相来源，生成物只存在于构建目录，不进入版本库。
#
# 已知重复：Protobuf 代码生成的写法与 src/gateway/CMakeLists.txt 重复。
# 本任务故意不做抽取——把公共 CMake 函数的重构混进「新增服务」的提交里会降低可
# 审阅性。等第三个 proto 出现、或把代码生成整体上移到顶层时一并处理，已记入
# docs/TASKS.md 的 Backlog。

# ---------------------------------------------------------------------------
# Protobuf 代码生成
# ---------------------------------------------------------------------------

find_package(Protobuf REQUIRED)
# vcpkg 的 brpc 以 unofficial-brpc 为包名导出，目标名是 unofficial::brpc::brpc。
find_package(unofficial-brpc CONFIG REQUIRED)

set(RGBT_MATCH_PROTO_DIR "${PROJECT_SOURCE_DIR}/api/proto")
set(RGBT_MATCH_PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto-gen")

file(MAKE_DIRECTORY "${RGBT_MATCH_PROTO_OUT_DIR}")

add_custom_command(
  OUTPUT "${RGBT_MATCH_PROTO_OUT_DIR}/match.pb.cc" "${RGBT_MATCH_PROTO_OUT_DIR}/match.pb.h"
  COMMAND protobuf::protoc
  ARGS
    --cpp_out "${RGBT_MATCH_PROTO_OUT_DIR}"
    --proto_path "${RGBT_MATCH_PROTO_DIR}"
    "${RGBT_MATCH_PROTO_DIR}/match.proto"
  DEPENDS "${RGBT_MATCH_PROTO_DIR}/match.proto"
  COMMENT "生成 match.proto 的 C++ 代码"
  VERBATIM
)

add_library(rgbt_match_proto STATIC "${RGBT_MATCH_PROTO_OUT_DIR}/match.pb.cc")
target_include_directories(rgbt_match_proto PUBLIC "${RGBT_MATCH_PROTO_OUT_DIR}")
target_link_libraries(rgbt_match_proto PUBLIC protobuf::libprotobuf)

# 生成代码属于第三方产出，不使用 -Werror 以免因 protoc 版本差异导致构建失败。
if(NOT MSVC)
  target_compile_options(rgbt_match_proto PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# 服务实现
# ---------------------------------------------------------------------------

add_library(
  rgbt_match_lib STATIC
  match_queue.cpp
  match_service.cpp
  room_allocator.cpp
)

target_include_directories(rgbt_match_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  rgbt_match_lib
  PUBLIC
    rgbt_common
    rgbt_match_proto
  PRIVATE
    unofficial::brpc::brpc
)

rgbt_set_project_warnings(rgbt_match_lib)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------

add_executable(rgbt_match match_main.cpp)
target_link_libraries(rgbt_match PRIVATE rgbt_match_lib unofficial::brpc::brpc)

set_target_properties(
  rgbt_match
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 本目录不定义测试目标，也不判断 GoogleTest 是否可用：测试放在 tests/unit/match/，
# 由 tests 目录自行解析测试依赖。这样服务目录只负责编译服务本身。
