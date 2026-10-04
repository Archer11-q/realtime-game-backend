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
///
/// dave 是第三个**启用**身份。它存在的理由不是「多一个测试账号」，而是有两条
/// 不变量必须三个启用身份才能验证：匹配的「第三个玩家不会被并入已配满的局」、
/// 房间的「非本局成员无法加入」。缺了它这两条只能在单元测试里覆盖，
/// 端到端脚本会一直打印「未覆盖项」。
constexpr BuiltinAccount kBuiltinAccounts[] = {
    {"alice", "alice_dev_pw"},
    {"bob", "bob_dev_pw"},
    {"carol", "carol_dev_pw"},
    {"dave", "dave_dev_pw"},
};

/// 压测合成账号的固定口令（TASK-022）。**开发凭据**，与 alice_dev_pw 同一性质。
constexpr const char* kSyntheticBenchPassword = "bench_dev_pw";

/// 压测合成账号的前缀与数字位数。
constexpr std::string_view kSyntheticBenchPrefix = "bench-";
constexpr std::size_t kSyntheticBenchDigits = 5;  // bench-00000 .. bench-99999

/// 合成账号是否已启用。默认关闭，由 Gateway 的命令行开关打开。
bool g_synthetic_bench_enabled = false;

/// 严格匹配 `bench-` + 恰好 5 位十进制数字。
///
/// 为什么严格到"恰好 5 位"：这是夹具约定，不是用户标识。宽松匹配（任意长度、
/// 允许非数字）会让 `bench-admin`、`benchmark` 这类名字意外获得一个固定口令，
/// 而那种错误在压测里完全看不出来。
bool IsSyntheticBenchAccount(std::string_view account) {
    if (account.size() != kSyntheticBenchPrefix.size() + kSyntheticBenchDigits) {
        return false;
    }
    if (account.compare(0, kSyntheticBenchPrefix.size(), kSyntheticBenchPrefix) != 0) {
        return false;
    }
    for (std::size_t i = kSyntheticBenchPrefix.size(); i < account.size(); ++i) {
        if (account[i] < '0' || account[i] > '9') {
            return false;
        }
    }
    return true;
}

/// 合成口令的摘要，只算一次（SHA-256 每次登录都算一遍是纯浪费）。
const std::string& SyntheticBenchPasswordHash() {
    static const std::string hash = HashPassword(kSyntheticBenchPassword);
    return hash;
}

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

/// @brief 压测合成账号是否已启用。供启动日志与测试使用。
[[nodiscard]] bool SyntheticBenchAccountsEnabled();

void EnableSyntheticBenchAccounts(bool enabled) {
    g_synthetic_bench_enabled = enabled;
}
bool SyntheticBenchAccountsEnabled() {
    return g_synthetic_bench_enabled;
}

std::string FindTestPasswordHash(std::string_view account) {
    for (const auto& credential : BuiltinTestCredentials()) {
        if (credential.account == account) {
            return credential.password_hash;
        }
    }
    // TASK-022：压测合成账号。只在显式开启时识认，且账号名**必须**严格匹配
    // `bench-<十进制数字>`——不做前缀宽松匹配，避免把 `benchmark`、
    // `bench-admin` 这类名字卷进来。
    if (SyntheticBenchAccountsEnabled() && IsSyntheticBenchAccount(account)) {
        return SyntheticBenchPasswordHash();
    }
    return {};
}

}  // namespace rgbt::gateway
