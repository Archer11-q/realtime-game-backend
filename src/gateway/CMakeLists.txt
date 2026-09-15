# brpc 冒烟验证（TASK-004 方案 C）
#
# 只用于验证 vcpkg 提供的 brpc 能否在本工程中编译、链接、启动并优雅退出。
# 默认构建（debug / release / asan 预设）不会包含本目录，因为 vcpkg 只在
# brpc-debug 预设中生效。
#
# 等 brpc 验证通过后，Gateway 正式实现会取代本目录的内容。

# vcpkg 的 brpc 以 unofficial-brpc 为包名导出，目标名是 unofficial::brpc::brpc。
# 必须先 find_package，否则链接时找不到该 IMPORTED 目标。
find_package(unofficial-brpc CONFIG REQUIRED)

add_executable(
  rgbt_brpc_smoke
  smoke_main.cpp
)

target_link_libraries(
  rgbt_brpc_smoke
  PRIVATE
    unofficial::brpc::brpc
    # 链接公共库：它不仅提供符号，也把 include/ 以 PUBLIC 方式传递给使用者。
    rgbt_common
)

# brpc 与 vcpkg 依赖的告警不在本项目控制范围内，因此这里只开基础告警，
# 不使用 rgbt_set_project_warnings（它带 -Werror，会因第三方头文件失败）。
if(NOT MSVC)
  target_compile_options(rgbt_brpc_smoke PRIVATE -Wall -Wextra)
endif()

set_target_properties(
  rgbt_brpc_smoke
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
