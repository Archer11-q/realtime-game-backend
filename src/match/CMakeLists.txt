# match 服务：TASK-007 匹配队列与配对；TASK-008 起改为真实分配房间。
#
# 依赖 vcpkg 提供的 brpc，因此本目录只在启用 vcpkg 工具链时加入
# （见 src/CMakeLists.txt），保证默认构建与 CI 不受影响。
#
# Protobuf 代码生成走公共函数 rgbt_add_service_proto（定义在
# cmake/generate_service_proto.cmake，顶层 CMakeLists.txt 已 include），
# 与 gateway / room 共用同一份逻辑。

# ---------------------------------------------------------------------------
# 依赖与代码生成
# ---------------------------------------------------------------------------

find_package(Protobuf REQUIRED)
# vcpkg 的 brpc 以 unofficial-brpc 为包名导出，目标名是 unofficial::brpc::brpc。
find_package(unofficial-brpc CONFIG REQUIRED)
# TASK-015：队列快照写入 Redis，需要 hiredis（与 gateway 用的是同一个包）。
find_package(hiredis CONFIG REQUIRED)

rgbt_add_service_proto(rgbt_match_proto match)

# ---------------------------------------------------------------------------
# 服务实现
# ---------------------------------------------------------------------------

add_library(
  rgbt_match_lib STATIC
  brpc_room_allocator.cpp
  match_queue.cpp
  match_queue_store.cpp
  match_service.cpp
  redis_match_queue_store.cpp
  room_allocator.cpp
)

target_include_directories(rgbt_match_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  rgbt_match_lib
  PUBLIC
    rgbt_common
    rgbt_match_proto
  PRIVATE
    # TASK-008 起 Match 通过 brpc 调用 RoomService 创建房间，
    # 需要 Room 的生成代码与服务桩。
    rgbt_room_proto
    unofficial::brpc::brpc
    # TASK-015：队列快照存储。
    hiredis::hiredis
)

rgbt_set_project_warnings(rgbt_match_lib)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------

add_executable(rgbt_match match_main.cpp)
target_link_libraries(
  rgbt_match
  PRIVATE
    rgbt_match_lib
    # `/metrics` 端点（TASK-019）。三个服务注册同一个实现，因此三个端口都能被
    # Prometheus 抓到，不需要额外的聚合进程。
    rgbt_common_metrics_service
    unofficial::brpc::brpc
)

# 服务可执行文件也要过 sanitizer，理由见 src/room/CMakeLists.txt 里的说明：
# 只给 *_lib 加 sanitizer 会让服务二进制链接失败，ASan 实际覆盖不到服务进程。
rgbt_apply_sanitizer(rgbt_match)

set_target_properties(
  rgbt_match
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 本目录不定义测试目标，也不判断 GoogleTest 是否可用：测试放在 tests/unit/match/，
# 由 tests 目录自行解析测试依赖。这样服务目录只负责编译服务本身。
