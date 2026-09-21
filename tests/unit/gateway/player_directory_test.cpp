/// @file player_directory_test.cpp
/// @brief 玩家身份目录的单元测试。
///
/// 覆盖两个实现：
///   * DatabasePlayerDirectory：用假 PlayerReader 注入，因此不需要 MySQL 即可
///     覆盖「档案不存在 / 数据库不可用 / 账号禁用」等难以在集成环境稳定复现的路径。
///   * InMemoryPlayerDirectory：内置测试身份的凭据校验。

#include "player_directory.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "database_player_directory.hpp"
#include "in_memory_player_directory.hpp"
#include "password_hash.hpp"
#include "player_reader.hpp"

namespace {

using rgbt::gateway::CredentialStatus;
using rgbt::gateway::DatabasePlayerDirectory;
using rgbt::gateway::InMemoryPlayerDirectory;
using rgbt::gateway::PlayerDirectory;
using rgbt::gateway::PlayerReader;
using rgbt::gateway::PlayerRecord;
using rgbt::gateway::ReaderStatus;
using rgbt::gateway::v1::PlayerInfo;

/// 内存玩家档案读取器，用于替代 MySQL。
class FakePlayerReader : public PlayerReader {
public:
    /// 置为 true 时所有读取返回 kUnavailable，用于模拟数据库不可用。
    bool unavailable = false;

    void Add(PlayerRecord record) { records_[record.account] = std::move(record); }

    ReaderStatus FindByAccount(std::string_view account,
                               std::optional<PlayerRecord>* out_record) override {
        if (unavailable) {
            return ReaderStatus::kUnavailable;
        }
        const auto it = records_.find(std::string(account));
        if (it == records_.end()) {
            return ReaderStatus::kNotFound;
        }
        *out_record = it->second;
        return ReaderStatus::kOk;
    }

    ReaderStatus FindByPlayerId(std::string_view player_id,
                                std::optional<PlayerRecord>* out_record) override {
        if (unavailable) {
            return ReaderStatus::kUnavailable;
        }
        for (const auto& [account, record] : records_) {
            if (record.player_id == player_id) {
                *out_record = record;
                return ReaderStatus::kOk;
            }
        }
        return ReaderStatus::kNotFound;
    }

private:
    std::map<std::string, PlayerRecord> records_;
};

PlayerRecord MakeRecord(const std::string& account, const std::string& player_id,
                        const std::string& display_name, const std::string& status) {
    PlayerRecord record;
    record.account = account;
    record.player_id = player_id;
    record.display_name = display_name;
    record.status = status;
    return record;
}

// ---------------------------------------------------------------------------
// DatabasePlayerDirectory：成功路径
// ---------------------------------------------------------------------------

TEST(DatabasePlayerDirectoryTest, AuthenticateSucceedsWithValidCredential) {
    FakePlayerReader reader;
    reader.Add(MakeRecord("alice", "p-0001", "Alice", "active"));
    DatabasePlayerDirectory directory(&reader);

    PlayerInfo player;
    // 密码来自 test_credentials.hpp 的测试身份。
    EXPECT_EQ(directory.Authenticate("alice", "alice_dev_pw", &player), CredentialStatus::kOk);
    EXPECT_EQ(player.player_id(), "p-0001");
    EXPECT_EQ(player.display_name(), "Alice");
    EXPECT_EQ(player.status(), "active");
}

// ---------------------------------------------------------------------------
// DatabasePlayerDirectory：失败路径
// ---------------------------------------------------------------------------

TEST(DatabasePlayerDirectoryTest, RejectsWrongPassword) {
    FakePlayerReader reader;
    reader.Add(MakeRecord("alice", "p-0001", "Alice", "active"));
    DatabasePlayerDirectory directory(&reader);

    PlayerInfo player;
    EXPECT_EQ(directory.Authenticate("alice", "wrong_password", &player),
              CredentialStatus::kInvalidCredential);
}

TEST(DatabasePlayerDirectoryTest, UnknownAccountReturnsSameReasonAsWrongPassword) {
    FakePlayerReader reader;
    reader.Add(MakeRecord("alice", "p-0001", "Alice", "active"));
    DatabasePlayerDirectory directory(&reader);

    PlayerInfo player;
    // 档案不存在与密码错误必须返回同一结果，避免泄露账号是否存在。
    EXPECT_EQ(directory.Authenticate("nobody", "whatever", &player),
              CredentialStatus::kInvalidCredential);
}

TEST(DatabasePlayerDirectoryTest, RejectsDisabledAccount) {
    FakePlayerReader reader;
    reader.Add(MakeRecord("carol", "p-0003", "Carol", "disabled"));
    DatabasePlayerDirectory directory(&reader);

    PlayerInfo player;
    EXPECT_EQ(directory.Authenticate("carol", "carol_dev_pw", &player),
              CredentialStatus::kAccountDisabled);
}

TEST(DatabasePlayerDirectoryTest, ReturnsUnavailableWhenReaderFails) {
    FakePlayerReader reader;
    reader.Add(MakeRecord("alice", "p-0001", "Alice", "active"));
    reader.unavailable = true;
    DatabasePlayerDirectory directory(&reader);

    PlayerInfo player;
    // 依赖故障必须与「凭据无效」区分：前者可重试，后者不可。
    EXPECT_EQ(directory.Authenticate("alice", "alice_dev_pw", &player),
              CredentialStatus::kUnavailable);
}

TEST(DatabasePlayerDirectoryTest, AccountInDatabaseWithoutTestCredentialIsRejected) {
    FakePlayerReader reader;
    // 档案里有 dave，但代码中没有它的测试凭据：属于配置不一致，按凭据无效处理。
    reader.Add(MakeRecord("dave", "p-0004", "Dave", "active"));
    DatabasePlayerDirectory directory(&reader);

    PlayerInfo player;
    EXPECT_EQ(directory.Authenticate("dave", "any_password", &player),
              CredentialStatus::kInvalidCredential);
}

TEST(DatabasePlayerDirectoryTest, NullReaderIsUnavailable) {
    DatabasePlayerDirectory directory(nullptr);
    PlayerInfo player;
    EXPECT_EQ(directory.Authenticate("alice", "alice_dev_pw", &player),
              CredentialStatus::kUnavailable);
}

// ---------------------------------------------------------------------------
// DatabasePlayerDirectory：FindByPlayerId
// ---------------------------------------------------------------------------

TEST(DatabasePlayerDirectoryTest, FindByPlayerIdReturnsProfile) {
    FakePlayerReader reader;
    reader.Add(MakeRecord("alice", "p-0001", "Alice", "active"));
    DatabasePlayerDirectory directory(&reader);

    const auto player = directory.FindByPlayerId("p-0001");
    ASSERT_TRUE(player.has_value());
    EXPECT_EQ(player->display_name(), "Alice");
}

TEST(DatabasePlayerDirectoryTest, FindByPlayerIdReturnsNulloptWhenMissing) {
    FakePlayerReader reader;
    DatabasePlayerDirectory directory(&reader);
    EXPECT_FALSE(directory.FindByPlayerId("p-9999").has_value());
}

TEST(DatabasePlayerDirectoryTest, FindByPlayerIdReturnsNulloptWhenReaderFails) {
    FakePlayerReader reader;
    reader.Add(MakeRecord("alice", "p-0001", "Alice", "active"));
    reader.unavailable = true;
    DatabasePlayerDirectory directory(&reader);
    // 依赖故障时按「查不到」处理，由上层根据错误路径决定对外语义。
    EXPECT_FALSE(directory.FindByPlayerId("p-0001").has_value());
}

// ---------------------------------------------------------------------------
// InMemoryPlayerDirectory
// ---------------------------------------------------------------------------

TEST(InMemoryPlayerDirectoryTest, BuiltinAccountsWork) {
    auto directory = PlayerDirectory::WithBuiltinTestAccounts();
    ASSERT_NE(directory, nullptr);

    PlayerInfo player;
    EXPECT_EQ(directory->Authenticate("bob", "bob_dev_pw", &player), CredentialStatus::kOk);
    EXPECT_EQ(player.player_id(), "p-0002");
}

TEST(InMemoryPlayerDirectoryTest, BuiltinDisabledAccountIsRejected) {
    auto directory = PlayerDirectory::WithBuiltinTestAccounts();
    PlayerInfo player;
    EXPECT_EQ(directory->Authenticate("carol", "carol_dev_pw", &player),
              CredentialStatus::kAccountDisabled);
}

TEST(InMemoryPlayerDirectoryTest, BuiltinWrongPasswordIsRejected) {
    auto directory = PlayerDirectory::WithBuiltinTestAccounts();
    PlayerInfo player;
    EXPECT_EQ(directory->Authenticate("alice", "nope", &player),
              CredentialStatus::kInvalidCredential);
}

TEST(InMemoryPlayerDirectoryTest, IsHealthyIsAlwaysTrue) {
    auto directory = PlayerDirectory::WithBuiltinTestAccounts();
    EXPECT_TRUE(directory->IsHealthy());
}

// ---------------------------------------------------------------------------
// 密码摘要
// ---------------------------------------------------------------------------

TEST(PasswordHashTest, ProducesStableSixtyFourCharHex) {
    const std::string first = rgbt::gateway::HashPassword("alice_dev_pw");
    const std::string second = rgbt::gateway::HashPassword("alice_dev_pw");
    EXPECT_EQ(first.size(), 64U);
    EXPECT_EQ(first, second);
    // 与外部工具计算的结果一致（sha256("alice_dev_pw")），
    // 用于确认实现没有被替换成其它摘要算法。
    EXPECT_EQ(first, "28400dcc49c78eb1d8bf6983d11e134d80e6f524e2bfde3908c691d6be4d3f35");
}

TEST(PasswordHashTest, DifferentPasswordsProduceDifferentHashes) {
    EXPECT_NE(rgbt::gateway::HashPassword("alice_dev_pw"),
              rgbt::gateway::HashPassword("bob_dev_pw"));
}

TEST(PasswordHashTest, ConstantTimeEqualsBehavesLikeEquality) {
    EXPECT_TRUE(rgbt::gateway::ConstantTimeEquals("abc", "abc"));
    EXPECT_FALSE(rgbt::gateway::ConstantTimeEquals("abc", "abd"));
    EXPECT_FALSE(rgbt::gateway::ConstantTimeEquals("abc", "ab"));
    EXPECT_TRUE(rgbt::gateway::ConstantTimeEquals("", ""));
}

}  // namespace
