/// @file error.hpp
/// @brief 错误码到 HTTP 状态码的映射。
///
/// 错误码定义在 api/proto/gateway.proto 的 ErrorCode 中，这里只提供它与 HTTP
/// 状态码之间的对应关系，避免各处在字符串上做判断。
///
/// 位置说明：本文件放在 gateway 目录而不是 common 目录，因为它依赖 gateway 的
/// 生成代码（gateway.pb.h）。若放进 common，会让公共库反向依赖网关的契约，
/// 形成错误的依赖方向。

#ifndef RGBT_GATEWAY_ERROR_HPP
#define RGBT_GATEWAY_ERROR_HPP

#include <cstdint>
#include <string_view>

#include "gateway.pb.h"

namespace rgbt::gateway {

/// @brief 把错误码映射为 HTTP 状态码。
///
/// 映射依据 docs/05-api-and-data.md 第 3 节的错误分类：
///   输入错误 -> 400，未认证 -> 401，资源不存在 -> 404，
///   暂时失败 -> 503，内部错误 -> 500。
[[nodiscard]] std::int32_t HttpStatusOf(rgbt::gateway::v1::ErrorCode code) noexcept;

/// @brief 返回错误码的稳定字符串名，用于日志与测试，避免依赖 protobuf 反射。
[[nodiscard]] std::string_view ErrorCodeName(rgbt::gateway::v1::ErrorCode code) noexcept;

/// @brief 判断错误是否属于「可有限重试」的暂时失败。
[[nodiscard]] bool IsRetryable(rgbt::gateway::v1::ErrorCode code) noexcept;

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_ERROR_HPP
