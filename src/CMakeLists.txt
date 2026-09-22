add_subdirectory(common)

# gateway 与 match 依赖 vcpkg 提供的 brpc。默认构建（debug / release / asan）不包含
# vcpkg，因此这里只在启用 vcpkg 工具链时加入，保证既有预设与 CI 不受影响。
#
# 顺序上 match 在 gateway 之前：gateway 会调用 MatchService（TASK-007 起），
# 先定义 rgbt_match_proto 让依赖关系在阅读时更直观。CMake 在生成阶段解析目标名，
# 因此顺序本身不是正确性要求。
if(RGBT_USE_VCPKG_TOOLCHAIN)
  add_subdirectory(match)
  add_subdirectory(gateway)
endif()
