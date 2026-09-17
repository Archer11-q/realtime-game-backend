#include "player_directory.hpp"

#include <utility>

namespace rgbt::gateway {

PlayerDirectory PlayerDirectory::WithBuiltinTestAccounts() {
    std::vector<PlayerAccount> accounts{
        PlayerAccount{"alice", "alice_dev_pw", "p-0001", "Alice", "active"},
        PlayerAccount{"bob", "bob_dev_pw", "p-0002", "Bob", "active"},
        // 刻意保留一个被禁用的账号，用于覆盖 active 之外的失败路径。
        PlayerAccount{"carol", "carol_dev_pw", "p-0003", "Carol", "disabled"},
    };
    return PlayerDirectory(std::move(accounts));
}

PlayerDirectory::PlayerDirectory(std::vector<PlayerAccount> accounts)
    : accounts_(std::move(accounts)) {}

CredentialStatus PlayerDirectory::Authenticate(std::string_view account, std::string_view password,
                                               rgbt::gateway::v1::PlayerInfo* player) const {
    if (player == nullptr) {
        return CredentialStatus::kInvalidCredential;
    }
    for (const auto& candidate : accounts_) {
        if (candidate.account != account) {
            continue;
        }
        if (candidate.password != password) {
            // 账号存在但密码错误，与账号不存在返回同一原因，避免泄露账号是否存在。
            return CredentialStatus::kInvalidCredential;
        }
        if (candidate.status != "active") {
            return CredentialStatus::kAccountDisabled;
        }
        player->set_player_id(candidate.player_id);
        player->set_display_name(candidate.display_name);
        player->set_status(candidate.status);
        return CredentialStatus::kOk;
    }
    return CredentialStatus::kInvalidCredential;
}

std::optional<rgbt::gateway::v1::PlayerInfo> PlayerDirectory::FindByPlayerId(
    std::string_view player_id) const {
    for (const auto& candidate : accounts_) {
        if (candidate.player_id == player_id) {
            rgbt::gateway::v1::PlayerInfo info;
            info.set_player_id(candidate.player_id);
            info.set_display_name(candidate.display_name);
            info.set_status(candidate.status);
            return info;
        }
    }
    return std::nullopt;
}

}  // namespace rgbt::gateway
