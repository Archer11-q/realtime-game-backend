/// @file test_credentials.hpp
/// @brief 内置测试身份的单一来源。
///
/// 依据 docs/07-open-decisions.md 的 D-002：
///   第一版使用测试账号，不实现注册系统，密码**不进入数据库**。
///
/// 本文件是这些测试身份的唯一来源，避免内存实现与数据库实现各自维护一份
/// 密码而逐渐不一致。
///
/// 与数据库的对应关系：迁移脚本 `migrations/004_seed_test_players.sql` 中的
/// account 与 player_id 必须与本文件一致，否则读数据库后鉴权会失败。
///
/// ⚠️ 这些不是生产凭据。正式实现必须替换为真实认证，并改用带盐的自适应哈希。

#ifndef RGBT_GATEWAY_TEST_CREDENTIALS_HPP
#define RGBT_GATEWAY_TEST_CREDENTIALS_HPP

#include <string>
#include <string_view>
#include <vector>

namespace rgbt::gateway {

/// 一条测试身份：账号与密码摘要。
struct TestCredential {
    std::string account;
    /// 明文密码的 SHA-256 摘要；代码中不保留明文常量。
    std::string password_hash;
};

/// @brief 返回内置测试身份列表（密码已哈希）。
[[nodiscard]] const std::vector<TestCredential>& BuiltinTestCredentials();

/// @brief 按账号查找测试身份的密码摘要；不存在时返回空。
[[nodiscard]] std::string FindTestPasswordHash(std::string_view account);

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_TEST_CREDENTIALS_HPP
