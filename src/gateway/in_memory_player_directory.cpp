#include "in_memory_player_directory.hpp"

#include <utility>

#include "password_hash.hpp"
#include "test_credentials.hpp"

namespace rgbt::gateway {
namespace {

/// 内置测试玩家的档案部分。
///
/// account 与 player_id 必须与 migrations/004_seed_test_players.sql 一致；
/// 密码来自 test_credentials.hpp，保持单一来源。
struct BuiltinProfile {
    const char* account;
    const char* player_id;
    const char* display_name;
    const char* status;
};

constexpr BuiltinProfile kBuiltinProfiles[] = {
    {"alice", "p-0001", "Alice", "active"},
    {"bob", "p-0002", "Bob", "active"},
    // 刻意保留禁用账号，用于覆盖失败路径。
    {"carol", "p-0003", "Carol", "disabled"},
};

}  // namespace

std::unique_ptr<PlayerDirectory> InMemoryPlayerDirectory::WithBuiltinAccounts() {
    std::vector<Entry> entries;
    entries.reserve(sizeof(kBuiltinProfiles) / sizeof(kBuiltinProfiles[0]));
    for (const auto& profile : kBuiltinProfiles) {
        Entry entry;
        entry.account = profile.account;
        entry.password_hash = FindTestPasswordHash(profile.account);
        entry.player_id = profile.player_id;
        entry.display_name = profile.display_name;
        entry.status = profile.status;
        entries.push_back(std::move(entry));
    }
    return std::make_unique<InMemoryPlayerDirectory>(std::move(entries));
}

std::unique_ptr<PlayerDirectory> PlayerDirectory::WithBuiltinTestAccounts() {
    return InMemoryPlayerDirectory::WithBuiltinAccounts();
}

InMemoryPlayerDirectory::InMemoryPlayerDirectory(std::vector<Entry> entries)
    : entries_(std::move(entries)) {}

CredentialStatus InMemoryPlayerDirectory::Authenticate(std::string_view account,
                                                       std::string_view password,
                                                       rgbt::gateway::v1::PlayerInfo* player) {
    if (player == nullptr) {
        return CredentialStatus::kInvalidCredential;
    }
    for (const auto& entry : entries_) {
        if (entry.account != account) {
            continue;
        }
        if (!ConstantTimeEquals(HashPassword(password), entry.password_hash)) {
            // 账号存在但密码错误：与账号不存在返回同一结果，避免泄露账号是否存在。
            return CredentialStatus::kInvalidCredential;
        }
        if (entry.status != "active") {
            return CredentialStatus::kAccountDisabled;
        }
        player->set_player_id(entry.player_id);
        player->set_display_name(entry.display_name);
        player->set_status(entry.status);
        return CredentialStatus::kOk;
    }
    return CredentialStatus::kInvalidCredential;
}

std::optional<rgbt::gateway::v1::PlayerInfo> InMemoryPlayerDirectory::FindByPlayerId(
    std::string_view player_id) {
    for (const auto& entry : entries_) {
        if (entry.player_id == player_id) {
            rgbt::gateway::v1::PlayerInfo info;
            info.set_player_id(entry.player_id);
            info.set_display_name(entry.display_name);
            info.set_status(entry.status);
            return info;
        }
    }
    return std::nullopt;
}

}  // namespace rgbt::gateway
