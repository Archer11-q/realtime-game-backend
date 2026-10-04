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

/// @brief 开启/关闭"压测合成账号"（TASK-022）。**默认关闭。**
///
/// 为什么需要它：内置测试身份只有 3 个启用账号（`alice`/`bob`/`dave`），而
/// **并发容量测试需要 N 个互不相同的玩家**。用 3 个账号跑 1000 连接会退化成
/// "3 个玩家反复配对"，测出来的不是容量——TASK-022 第一轮就这么踩了：1000 个
/// 机器人只产生了 3 个 `player_id`，于是绝大多数入队被幂等判为 `kAlreadyQueued`，
/// 而同一玩家的重复进房又是**幂等成功**，最终表现为"667 次 join 挤进同一个房间"
/// 这种自相矛盾的数字。
///
/// 规则（显式、可审计、与内置身份同一性质）：账号名形如 `bench-<十进制数字>`
/// （如 `bench-00042`）时，口令为固定的 `bench_dev_pw`。二者都是**开发凭据**，
/// 与 `alice_dev_pw` 一样不是生产认证。
///
/// 未调用本函数时 `bench-*` 一律不识认，行为与今天完全一致——因此生产路径不受
/// 影响，而压测脚本必须显式打开 `-enable_bench_accounts` 才能用它们。
void EnableSyntheticBenchAccounts(bool enabled);

/// @brief 压测合成账号是否已启用。供启动日志与测试使用。
[[nodiscard]] bool SyntheticBenchAccountsEnabled();

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_TEST_CREDENTIALS_HPP
