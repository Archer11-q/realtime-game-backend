# common：跨服务共享的最小基础设施。
#
# 当前包含版本信息与 Token 生成，两者都被多个服务复用。
# 每个新增的公共模块都必须有对应测试，避免 common 变成无测试的杂物间。

add_library(
  rgbt_common STATIC
  logging.cpp
  metrics.cpp
  token.cpp
  version.cpp
)

target_include_directories(rgbt_common PUBLIC "${PROJECT_SOURCE_DIR}/include")

# 版本号由构建系统注入，保证与 project(VERSION ...) 单一来源。
# 使用编译期宏而不是 @VAR@ 模板头，避免格式化工具破坏占位符。
target_compile_definitions(
  rgbt_common
  PUBLIC
    RGBT_VERSION_STRING="${PROJECT_VERSION}"
    RGBT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    RGBT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    RGBT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

rgbt_set_project_warnings(rgbt_common)

# ---------------------------------------------------------------------------
# MySQL 连接辅助
# ---------------------------------------------------------------------------
#
# 本模块原在 src/gateway/。TASK-008 的 Room/Battle Service 也要写 match_results，
# 于是它成为被两个服务使用的代码，按 docs/01-architecture.md 第 11 节的放置规则
# 移入 include/common/。
#
# 但它依赖 vcpkg 提供的 libmariadb，而默认预设（debug/release/asan）没有 vcpkg，
# 因此**不并入 rgbt_common**，而是单独成一个目标，只在启用 vcpkg 时构建。
# 否则三个默认预设会因为找不到 mariadb 头文件而配置失败。

# ---------------------------------------------------------------------------
# `/metrics` 端点（TASK-019）
# ---------------------------------------------------------------------------
#
# 与上面两个模块的区别：它依赖 brpc。默认预设（debug/release/asan）没有 vcpkg，
# 因此不能并入 rgbt_common，否则那三个预设会因为找不到 brpc 头文件而配置失败
# （与 rgbt_common_mysql 同一考虑）。
#
# 三个服务的业务库都 PUBLIC 链接 rgbt_common；本目标必须**单独链接**到
# 各服务的可执行目标上，理由与 docs/01-architecture.md 第 11 节的放置规则一致：
# 只有真正需要 brpc 的地方才引入它。

if(RGBT_USE_VCPKG_TOOLCHAIN)
  find_package(Protobuf REQUIRED)
  # 必须先 find_package，否则下面链接时找不到 `unofficial::brpc::brpc`
  # 这个 IMPORTED 目标。三个服务目录各自也做了同样的调用（CMake 允许重复）。
  find_package(unofficial-brpc CONFIG REQUIRED)
  rgbt_add_service_proto(rgbt_metrics_proto metrics)

  add_library(rgbt_common_metrics_service STATIC metrics_service.cpp)

  target_include_directories(
    rgbt_common_metrics_service PUBLIC "${PROJECT_SOURCE_DIR}/include"
  )

  target_link_libraries(
    rgbt_common_metrics_service
    PUBLIC
      rgbt_common
      rgbt_metrics_proto
    PRIVATE
      unofficial::brpc::brpc
  )

  rgbt_set_project_warnings(rgbt_common_metrics_service)
endif()

if(RGBT_USE_VCPKG_TOOLCHAIN)
  find_package(unofficial-libmariadb CONFIG REQUIRED)

  add_library(rgbt_common_mysql STATIC mysql_connection.cpp)

  target_include_directories(rgbt_common_mysql PUBLIC "${PROJECT_SOURCE_DIR}/include")

  target_link_libraries(rgbt_common_mysql PUBLIC rgbt_common PRIVATE unofficial::libmariadb)

  rgbt_set_project_warnings(rgbt_common_mysql)
endif()
