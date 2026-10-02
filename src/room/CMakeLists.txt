# room 服务：TASK-008 房间生命周期与权威状态。
#
# 依赖 vcpkg 提供的 brpc 与 libmariadb，因此本目录只在启用 vcpkg 工具链时加入
# （见 src/CMakeLists.txt），保证默认构建与 CI 不受影响。
#
# Protobuf 代码生成走公共函数 rgbt_add_service_proto（定义在
# cmake/generate_service_proto.cmake，顶层 CMakeLists.txt 已 include），
# 与 gateway / match 共用同一份逻辑——本目录是第三个 proto，触发了
# docs/TASKS.md Backlog 里记录的抽取条件。

find_package(Protobuf REQUIRED)
# vcpkg 的 brpc 以 unofficial-brpc 为包名导出，目标名是 unofficial::brpc::brpc。
find_package(unofficial-brpc CONFIG REQUIRED)

rgbt_add_service_proto(rgbt_room_proto room)

# ---------------------------------------------------------------------------
# 服务实现
# ---------------------------------------------------------------------------

add_library(
  rgbt_room_lib STATIC
  battle_room.cpp
  mysql_match_result_writer.cpp
  room_manager.cpp
  room_service.cpp
)

target_include_directories(rgbt_room_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  rgbt_room_lib
  PUBLIC
    rgbt_common
    # MySQL 连接辅助原在 gateway 内，TASK-008 起被 Room 与 Gateway 两个服务使用，
    # 因此按放置规则移入 include/common/ 并单独成目标（见 src/common/CMakeLists.txt）。
    rgbt_common_mysql
    rgbt_room_proto
  PRIVATE
    unofficial::brpc::brpc
)

rgbt_set_project_warnings(rgbt_room_lib)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------

add_executable(rgbt_room room_main.cpp)
target_link_libraries(rgbt_room PRIVATE rgbt_room_lib unofficial::brpc::brpc)

set_target_properties(
  rgbt_room
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 本目录不定义测试目标，也不判断 GoogleTest 是否可用：测试放在 tests/unit/room/，
# 由 tests 目录自行解析测试依赖。这样服务目录只负责编译服务本身。
