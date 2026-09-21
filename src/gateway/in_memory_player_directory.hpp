/// @file in_memory_player_directory.hpp
/// @brief 内存实现的玩家身份目录（不访问数据库）。
///
/// 用途：
///   * 单元测试：覆盖账号禁用、密码错误、档案不存在等路径。
///   * 未配置数据库时的本地快速验证。
///
/// 注意：它读取的是代码内的测试身份，与 players 表**不是**同一来源。
/// 正常服务路径应使用 DatabasePlayerDirectory（读 MySQL）。

#ifndef RGBT_GATEWAY_IN_MEMORY_PLAYER_DIRECTORY_HPP
#define RGBT_GATEWAY_IN_MEMORY_PLAYER_DIRECTORY_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "player_directory.hpp"

namespace rgbt::gateway {

class InMemoryPlayerDirectory : public PlayerDirectory {
public:
    /// 使用内置测试身份构造（与迁移脚本的种子数据一致）。
    static std::unique_ptr<PlayerDirectory> WithBuiltinAccounts();

    /// 完整记录：凭据摘要 + 档案。仅供内存实现使用。
    struct Entry {
        std::string account;
        std::string password_hash;
        std::string player_id;
        std::string display_name;
        std::string status;
    };

    /// 使用显式提供的记录构造，便于测试构造特定场景。
    explicit InMemoryPlayerDirectory(std::vector<Entry> entries);

    CredentialStatus Authenticate(std::string_view account, std::string_view password,
                                  rgbt::gateway::v1::PlayerInfo* player) override;

    [[nodiscard]] std::optional<rgbt::gateway::v1::PlayerInfo> FindByPlayerId(
        std::string_view player_id) override;

    [[nodiscard]] bool IsHealthy() override { return true; }

private:
    std::vector<Entry> entries_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_IN_MEMORY_PLAYER_DIRECTORY_HPP
