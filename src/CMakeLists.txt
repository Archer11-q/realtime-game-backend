add_subdirectory(common)

# gateway、match 与 room 依赖 vcpkg 提供的 brpc。默认构建（debug / release / asan）
# 不包含 vcpkg，因此这里只在启用 vcpkg 工具链时加入，保证既有预设与 CI 不受影响。
#
# 顺序：room 最先，match 次之，gateway 最后。
#   Match 调用 RoomService 创建房间（TASK-008），Gateway 同时依赖 Match 与 Room
#   的契约。把被依赖方放在前面，让阅读顺序与调用方向一致。
#   CMake 在生成阶段解析目标名，因此顺序本身不是正确性要求。
if(RGBT_USE_VCPKG_TOOLCHAIN)
  add_subdirectory(room)
  add_subdirectory(match)
  add_subdirectory(gateway)
endif()
