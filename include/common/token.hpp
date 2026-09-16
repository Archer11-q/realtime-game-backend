/// @file token.hpp
/// @brief 会话 Token 生成。
///
/// 设计（对应 docs/07-open-decisions.md D-002 的选择）：
///   * 使用不透明随机串，而不是 JWT。第一版不需要 JWT 的自包含性，
///     会话状态集中在 Redis 中，便于统一吊销与过期控制。
///   * 使用 std::random_device 播种的 mt19937_64 生成随机字节，再按 URL 安全
///     字母表编码。注意：这是**开发期实现**，强度满足本地与演示需求；
///     若将来用于生产，需要评估改用操作系统 CSPRNG（例如 getrandom）。
///   * Token 只由服务端生成，客户端传入的 Token 一律视为不可信输入。

#ifndef RGBT_COMMON_TOKEN_HPP
#define RGBT_COMMON_TOKEN_HPP

#include <cstddef>
#include <string>

namespace rgbt::common {

/// Token 的字符长度。默认 32 个字符，约 160 bit 熵。
inline constexpr std::size_t kDefaultTokenLength = 32;

/// Token 允许出现的字符集合（URL 安全，无需转义）。
inline constexpr const char* kTokenAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// @brief 生成一个会话 Token。
/// @param length 字符长度，取 1..256；超出范围时按边界截断。
[[nodiscard]] std::string GenerateToken(std::size_t length = kDefaultTokenLength);

/// @brief 校验 Token 是否只由允许的字符组成且长度合法。
///
/// 用途：在查询 Redis 之前先做格式校验，避免把任意客户端输入直接当作 Key 使用。
[[nodiscard]] bool IsValidTokenFormat(const std::string& token) noexcept;

}  // namespace rgbt::common

#endif  // RGBT_COMMON_TOKEN_HPP
