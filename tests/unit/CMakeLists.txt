# 单元测试。
#
# 各服务的测试放在各自子目录（与 src/<service>/ 对应），便于按服务定位。
#
# gateway 测试依赖 rgbt_gateway_lib，而该目标只在启用 vcpkg 工具链时才存在
# （它需要 brpc 与 hiredis）。默认预设（debug/release/asan）不包含 gateway，
# 因此这里必须按目标是否存在来决定是否加入其测试目录，否则会因
# 「target 未被本项目构建」而在配置阶段报错。

add_subdirectory(common)

if(TARGET rgbt_gateway_lib)
  add_subdirectory(gateway)
else()
  message(
    STATUS
      "未启用 vcpkg 工具链（缺少 rgbt_gateway_lib），跳过 gateway 单元测试。"
      " 需要时使用 brpc-debug 预设。"
  )
endif()
