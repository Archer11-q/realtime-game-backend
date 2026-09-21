#include "database_player_directory.hpp"

#include "password_hash.hpp"
#include "test_credentials.hpp"

namespace rgbt::gateway {
namespace {

/// 把档案记录转换为对外返回的 PlayerInfo。
rgbt::gateway::v1::PlayerInfo ToPlayerInfo(const PlayerRecord& record) {
    rgbt::gateway::v1::PlayerInfo info;
    info.set_player_id(record.player_id);
    info.set_display_name(record.display_name);
    info.set_status(record.status);
    return info;
}

}  // namespace

DatabasePlayerDirectory::DatabasePlayerDirectory(PlayerReader* reader) : reader_(reader) {}

CredentialStatus DatabasePlayerDirectory::Authenticate(std::string_view account,
                                                       std::string_view password,
                                                       rgbt::gateway::v1::PlayerInfo* player) {
    if (reader_ == nullptr || player == nullptr) {
        return CredentialStatus::kUnavailable;
    }

    std::optional<PlayerRecord> record;
    const ReaderStatus status = reader_->FindByAccount(account, &record);
    if (status == ReaderStatus::kUnavailable) {
        // 依赖故障与「账号不存在」必须区分：前者可重试，后者不可。
        return CredentialStatus::kUnavailable;
    }
    if (status == ReaderStatus::kNotFound || !record.has_value()) {
        // 档案不存在：与密码错误返回同一结果，避免泄露账号是否存在。
        return CredentialStatus::kInvalidCredential;
    }

    const std::string expected_hash = FindTestPasswordHash(account);
    if (expected_hash.empty()) {
        // players 表里有该账号，但代码中没有对应测试凭据。
        // 这属于数据与配置不一致，按凭据无效处理，不向客户端暴露细节。
        return CredentialStatus::kInvalidCredential;
    }
    if (!ConstantTimeEquals(HashPassword(password), expected_hash)) {
        return CredentialStatus::kInvalidCredential;
    }

    if (record->status != "active") {
        return CredentialStatus::kAccountDisabled;
    }

    *player = ToPlayerInfo(*record);
    return CredentialStatus::kOk;
}

std::optional<rgbt::gateway::v1::PlayerInfo> DatabasePlayerDirectory::FindByPlayerId(
    std::string_view player_id) {
    if (reader_ == nullptr) {
        return std::nullopt;
    }
    std::optional<PlayerRecord> record;
    if (reader_->FindByPlayerId(player_id, &record) != ReaderStatus::kOk || !record.has_value()) {
        return std::nullopt;
    }
    return ToPlayerInfo(*record);
}

bool DatabasePlayerDirectory::IsHealthy() {
    return reader_ != nullptr;
}

}  // namespace rgbt::gateway
