# common：跨服务共享的最小基础设施。
#
# 当前包含版本信息与 Token 生成，两者都被多个服务复用。
# 每个新增的公共模块都必须有对应测试，避免 common 变成无测试的杂物间。

add_library(
  rgbt_common STATIC
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
