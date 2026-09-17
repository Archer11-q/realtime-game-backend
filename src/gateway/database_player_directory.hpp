/// @file database_player_directory.hpp
/// @brief 基于 MySQL 玩家档案 + 代码内测试凭据的身份目录。
///
/// 数据来源组合：
///   * 玩家档案（player_id / display_name / status）：来自 players 表。
///   * 账号密码：来自代码中的测试身份，以 SHA-256 摘要比较（见 D-002）。
///
/// 因此校验流程是：先按 account 读档案，再比对密码摘要，最后检查 status。
/// 这样做的意义是让档案真的走数据库，而不是继续用代码里的常量——只有真实
/// 读写才能验证表结构是否合理。
///
/// 退出条件提示：Phase 1 期间 Gateway 直接读 players 表，依据 ADR-0002；
/// Player/State 服务落地后本类应被替换为对该服务的 RPC 调用。

#ifndef RGBT_GATEWAY_DATABASE_PLAYER_DIRECTORY_HPP
#define RGBT_GATEWAY_DATABASE_PLAYER_DIRECTORY_HPP

#include <optional>
#include <string_view>

#include "player_directory.hpp"
#include "player_reader.hpp"

namespace rgbt::gateway {

class DatabasePlayerDirectory : public PlayerDirectory {
public:
    /// 不接管 reader 的所有权，调用方需保证其生命周期覆盖本对象。
    explicit DatabasePlayerDirectory(PlayerReader* reader);

    CredentialStatus Authenticate(std::string_view account, std::string_view password,
                                  rgbt::gateway::v1::PlayerInfo* player) override;

    [[nodiscard]] std::optional<rgbt::gateway::v1::PlayerInfo> FindByPlayerId(
        std::string_view player_id) override;

    [[nodiscard]] bool IsHealthy() override;

private:
    PlayerReader* reader_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_DATABASE_PLAYER_DIRECTORY_HPP
