#include "test_credentials.hpp"

#include "password_hash.hpp"

namespace rgbt::gateway {
namespace {

/// 测试身份的档案部分。
///
/// account 与 player_id 必须与 migrations/004_seed_test_players.sql 一致。
/// 密码只在这里出现一次，且立即转成摘要，不留明文常量在别处。
struct BuiltinAccount {
    const char* account;
    const char* password;
};

/// 刻意包含一个 disabled 账号（carol），用于覆盖「账号被禁用」这条失败路径。
constexpr BuiltinAccount kBuiltinAccounts[] = {
    {"alice", "alice_dev_pw"},
    {"bob", "bob_dev_pw"},
    {"carol", "carol_dev_pw"},
};

}  // namespace

const std::vector<TestCredential>& BuiltinTestCredentials() {
    // 只构造一次：哈希在首次调用时完成，之后复用。
    static const std::vector<TestCredential> credentials = [] {
        std::vector<TestCredential> result;
        result.reserve(sizeof(kBuiltinAccounts) / sizeof(kBuiltinAccounts[0]));
        for (const auto& account : kBuiltinAccounts) {
            result.push_back(TestCredential{account.account, HashPassword(account.password)});
        }
        return result;
    }();
    return credentials;
}

std::string FindTestPasswordHash(std::string_view account) {
    for (const auto& credential : BuiltinTestCredentials()) {
        if (credential.account == account) {
            return credential.password_hash;
        }
    }
    return {};
}

}  // namespace rgbt::gateway
