# 单元测试。
#
# 各服务的测试放在各自子目录（与 src/<service>/ 对应），便于按服务定位。
#
# gateway 与 match 测试依赖只在启用 vcpkg 工具链时才存在的目标
# （rgbt_gateway_lib / rgbt_match_lib，它们需要 brpc 等依赖）。默认预设
# （debug/release/asan）不包含这两个服务，因此这里必须按目标是否存在来决定是否
# 加入其测试目录，否则会因「target 未被本项目构建」而在配置阶段报错。

add_subdirectory(common)

if(TARGET rgbt_match_lib)
  add_subdirectory(match)
else()
  message(
    STATUS
      "未启用 vcpkg 工具链（缺少 rgbt_match_lib），跳过 match 单元测试。"
      " 需要时使用 brpc-debug 预设。"
  )
endif()

if(TARGET rgbt_gateway_lib)
  add_subdirectory(gateway)
else()
  message(
    STATUS
      "未启用 vcpkg 工具链（缺少 rgbt_gateway_lib），跳过 gateway 单元测试。"
      " 需要时使用 brpc-debug 预设。"
  )
endif()
