/// @file password_hash.hpp
/// @brief 测试密码的哈希与校验。
///
/// 背景（依据 docs/07-open-decisions.md 的 D-002）：
///   第一版不实现注册系统，账号密码不进入数据库。但把测试密码以明文常量形式
///   留在代码里，会让人误以为「密码已经处理好了」，而且未来迁移到真实认证时
///   这段逻辑要重写。
///
/// 因此本模块用 SHA-256 对密码做哈希后比较，而不是直接比较明文：
///   * 代码中不出现明文密码常量，改为运行时对测试密码求哈希；
///   * 比较使用恒定时间实现，避免通过响应时间推断密码；
///   * 依赖 openssl 已在项目依赖中（brpc 引入），不新增组件。
///
/// 明确的局限：SHA-256 是快速哈希，**不适合真实账号体系**。正式实现必须改用
/// 带盐的自适应算法（如 bcrypt / Argon2 / scrypt）。本模块只为测试身份服务，
/// 落地真实认证时必须替换，并新增 ADR。

#ifndef RGBT_GATEWAY_PASSWORD_HASH_HPP
#define RGBT_GATEWAY_PASSWORD_HASH_HPP

#include <string>
#include <string_view>

namespace rgbt::gateway {

/// @brief 计算明文密码的 SHA-256 十六进制摘要（小写，64 字符）。
[[nodiscard]] std::string HashPassword(std::string_view password);

/// @brief 以恒定时间比较两个摘要，避免基于比较耗时的时间侧信道。
[[nodiscard]] bool ConstantTimeEquals(std::string_view lhs, std::string_view rhs) noexcept;

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_PASSWORD_HASH_HPP
