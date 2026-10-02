# 为 api/proto/<name>.proto 生成 C++ 代码，并创建一个静态库目标。
#
# 为什么抽成公共函数：TASK-007 引入第二个 proto 时，src/gateway 与 src/match 的
# Protobuf 代码生成写法就已经完全重复。docs/TASKS.md 的 Backlog 当时记录了
# 「建议在第三个 proto 出现时一并处理」。TASK-008 引入 api/proto/room.proto，
# 即第三个，因此在此抽取，三个服务共用同一份生成逻辑。
#
# 契约文件放在 api/proto/ 是接口真相来源；生成物只存在于构建目录，不进入版本库。
#
# 用法：
#   rgbt_add_service_proto(<target> <proto_basename>)
#
# 例如 rgbt_add_service_proto(rgbt_room_proto room) 会：
#   * 生成 ${CMAKE_CURRENT_BINARY_DIR}/proto-gen/room.pb.{h,cc}
#   * 创建静态库目标 rgbt_room_proto，PUBLIC 暴露生成目录与 protobuf::libprotobuf
#     （因此链接它的服务不需要再手写 include 目录）
#   * 对生成代码关闭告警：它属于第三方产出，不应因 protoc 版本差异而触发 -Werror
#
# 调用方仍需自行 find_package(Protobuf REQUIRED)。

function(rgbt_add_service_proto target proto_name)
  set(proto_source_dir "${PROJECT_SOURCE_DIR}/api/proto")
  set(proto_out_dir "${CMAKE_CURRENT_BINARY_DIR}/proto-gen")

  if(NOT EXISTS "${proto_source_dir}/${proto_name}.proto")
    message(FATAL_ERROR "找不到契约文件 ${proto_source_dir}/${proto_name}.proto")
  endif()

  file(MAKE_DIRECTORY "${proto_out_dir}")

  add_custom_command(
    OUTPUT "${proto_out_dir}/${proto_name}.pb.cc" "${proto_out_dir}/${proto_name}.pb.h"
    COMMAND protobuf::protoc
    ARGS
      --cpp_out "${proto_out_dir}"
      --proto_path "${proto_source_dir}"
      "${proto_source_dir}/${proto_name}.proto"
    DEPENDS "${proto_source_dir}/${proto_name}.proto"
    COMMENT "生成 ${proto_name}.proto 的 C++ 代码"
    VERBATIM
  )

  add_library(${target} STATIC "${proto_out_dir}/${proto_name}.pb.cc")
  target_include_directories(${target} PUBLIC "${proto_out_dir}")
  target_link_libraries(${target} PUBLIC protobuf::libprotobuf)

  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -w)
  endif()
endfunction()
