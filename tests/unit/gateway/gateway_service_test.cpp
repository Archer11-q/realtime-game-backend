/// @file gateway_service_test.cpp
/// @brief Gateway 登录切片的单元测试。
///
/// 用内存假存储注入 SessionStore，因此这些用例不需要 Redis 即可运行，
/// 并且可以精确模拟「Redis 不可用」这类难以在集成环境稳定复现的路径。

#include "gateway_service.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>

#include "common/token.hpp"
#include "error.hpp"
#include "player_directory.hpp"
#include "session_store.hpp"

namespace {

using rgbt::gateway::CredentialStatus;
using rgbt::gateway::GatewayServiceImpl;
using rgbt::gateway::PlayerDirectory;
using rgbt::gateway::SessionRecord;
using rgbt::gateway::SessionStore;
using rgbt::gateway::StoreStatus;
using rgbt::gateway::v1::ErrorCode;
using rgbt::gateway::v1::GetCurrentPlayerRequest;
using rgbt::gateway::v1::GetCurrentPlayerResponse;
using rgbt::gateway::v1::LoginRequest;
using rgbt::gateway::v1::LoginResponse;
using rgbt::gateway::v1::LogoutRequest;
using rgbt::gateway::v1::LogoutResponse;

/// 内存会话存储，行为对齐 Redis 实现的语义（含 request_id 幂等）。
class FakeSessionStore : public SessionStore {
public:
    /// 置为 true 时所有操作返回 kUnavailable，用于模拟 Redis 不可用。
    bool unavailable = false;

    /// 置为 true 时 CreateSession 直接失败，用于区分「读不可用」与「写不可用」。
    bool fail_writes = false;

    StoreStatus CreateSession(const std::string& request_id, const std::string& player_id,
                              const std::string& client_type, std::int32_t ttl_seconds,
                              std::string* out_token) override {
        // 假存储不实现 TTL，但保留参数以符合接口。
        (void)ttl_seconds;
        if (unavailable || fail_writes) {
            return StoreStatus::kUnavailable;
        }
        if (!request_id.empty()) {
            const auto it = idempotency_.find(request_id);
            if (it != idempotency_.end()) {
                *out_token = it->second;
                return StoreStatus::kOk;
            }
        }
        const std::string token = rgbt::common::GenerateToken();
        SessionRecord record;
        record.token = token;
        record.player_id = player_id;
        record.client_type = client_type;
        sessions_[token] = record;
        if (!request_id.empty()) {
            idempotency_[request_id] = token;
        }
        *out_token = token;
        return StoreStatus::kOk;
    }

    StoreStatus GetSession(const std::string& token, SessionRecord* out_session) override {
        if (unavailable) {
            return StoreStatus::kUnavailable;
        }
        const auto it = sessions_.find(token);
        if (it == sessions_.end()) {
            return StoreStatus::kNotFound;
        }
        *out_session = it->second;
        return StoreStatus::kOk;
    }

    StoreStatus DeleteSession(const std::string& token) override {
        if (unavailable) {
            return StoreStatus::kUnavailable;
        }
        const auto erased = sessions_.erase(token);
        return erased > 0 ? StoreStatus::kOk : StoreStatus::kNotFound;
    }

    std::size_t session_count() const { return sessions_.size(); }

private:
    std::map<std::string, SessionRecord> sessions_;
    std::map<std::string, std::string> idempotency_;
};

/// 测试夹具：构造服务与依赖。
class GatewayServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // PlayerDirectory 现在是接口，需要以指针持有（内存实现，不访问数据库）。
        players_ = PlayerDirectory::WithBuiltinTestAccounts();
        service_ = std::make_unique<GatewayServiceImpl>(&sessions_, players_.get());
    }

    /// 构造一个只填写必填字段的合法登录请求。
    static LoginRequest MakeValidLogin(const std::string& account = "alice",
                                       const std::string& password = "alice_dev_pw",
                                       const std::string& request_id = "") {
        LoginRequest request;
        request.set_account(account);
        request.set_password(password);
        request.set_request_id(request_id);
        return request;
    }

    FakeSessionStore sessions_;
    std::unique_ptr<PlayerDirectory> players_;
    std::unique_ptr<GatewayServiceImpl> service_;
};

// ---------------------------------------------------------------------------
// 登录：成功路径
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, LoginSucceedsWithValidCredential) {
    LoginRequest request = MakeValidLogin();
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200);
    EXPECT_FALSE(response.has_error());
    EXPECT_FALSE(response.token().empty());
    EXPECT_TRUE(rgbt::common::IsValidTokenFormat(response.token()));
    EXPECT_GT(response.expires_in_seconds(), 0);
    EXPECT_EQ(response.player().player_id(), "p-0001");
    EXPECT_EQ(response.player().display_name(), "Alice");
    EXPECT_EQ(sessions_.session_count(), 1U);
}

TEST_F(GatewayServiceTest, LoginDefaultsClientTypeToWeb) {
    LoginRequest request = MakeValidLogin();
    request.set_client_type("");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    ASSERT_EQ(response.status_code(), 200);
    SessionRecord record;
    ASSERT_EQ(sessions_.GetSession(response.token(), &record), StoreStatus::kOk);
    EXPECT_EQ(record.client_type, "web");
}

// ---------------------------------------------------------------------------
// 登录：输入错误
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, LoginRejectsEmptyAccount) {
    LoginRequest request = MakeValidLogin();
    request.set_account("");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(response.error().reason(), "account_required");
    // proto3 的 string 字段没有 has_token()，用取值判断是否未返回 Token。
    EXPECT_TRUE(response.token().empty());
}

TEST_F(GatewayServiceTest, LoginRejectsEmptyPassword) {
    LoginRequest request = MakeValidLogin();
    request.set_password("");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().reason(), "password_required");
}

TEST_F(GatewayServiceTest, LoginRejectsOverlongAccount) {
    LoginRequest request = MakeValidLogin();
    request.set_account(std::string(65, 'a'));
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().reason(), "account_too_long");
}

TEST_F(GatewayServiceTest, LoginRejectsUnknownClientType) {
    LoginRequest request = MakeValidLogin();
    request.set_client_type("mobile");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().reason(), "unsupported_client_type");
}

// ---------------------------------------------------------------------------
// 登录：凭据错误
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, LoginRejectsWrongPassword) {
    LoginRequest request = MakeValidLogin("alice", "wrong_password");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 401);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().code(), ErrorCode::UNAUTHENTICATED);
    EXPECT_EQ(response.error().reason(), "invalid_credential");
    EXPECT_EQ(sessions_.session_count(), 0U);
}

TEST_F(GatewayServiceTest, LoginRejectsUnknownAccountWithSameReasonAsWrongPassword) {
    LoginRequest request = MakeValidLogin("nobody", "whatever");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 401);
    ASSERT_TRUE(response.has_error());
    // 账号不存在与密码错误必须返回同一原因，避免泄露账号是否存在。
    EXPECT_EQ(response.error().reason(), "invalid_credential");
}

TEST_F(GatewayServiceTest, LoginRejectsDisabledAccount) {
    LoginRequest request = MakeValidLogin("carol", "carol_dev_pw");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 401);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().reason(), "account_disabled");
    EXPECT_EQ(sessions_.session_count(), 0U);
}

// ---------------------------------------------------------------------------
// 登录：幂等
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, LoginIsIdempotentForSameRequestId) {
    LoginRequest request = MakeValidLogin("alice", "alice_dev_pw", "req-001");

    LoginResponse first;
    service_->Login(nullptr, &request, &first, nullptr);
    LoginResponse second;
    service_->Login(nullptr, &request, &second, nullptr);

    ASSERT_EQ(first.status_code(), 200);
    ASSERT_EQ(second.status_code(), 200);
    EXPECT_EQ(first.token(), second.token());
    EXPECT_EQ(sessions_.session_count(), 1U);
}

TEST_F(GatewayServiceTest, LoginWithDifferentRequestIdCreatesDifferentSession) {
    LoginRequest first_request = MakeValidLogin("alice", "alice_dev_pw", "req-001");
    LoginRequest second_request = MakeValidLogin("alice", "alice_dev_pw", "req-002");

    LoginResponse first;
    service_->Login(nullptr, &first_request, &first, nullptr);
    LoginResponse second;
    service_->Login(nullptr, &second_request, &second, nullptr);

    ASSERT_EQ(first.status_code(), 200);
    ASSERT_EQ(second.status_code(), 200);
    EXPECT_NE(first.token(), second.token());
    EXPECT_EQ(sessions_.session_count(), 2U);
}

// ---------------------------------------------------------------------------
// 登录：依赖不可用
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, LoginReturnsUnavailableWhenStoreFails) {
    sessions_.unavailable = true;
    LoginRequest request = MakeValidLogin();
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().code(), ErrorCode::UNAVAILABLE);
    EXPECT_EQ(response.error().reason(), "session_store_unavailable");
    // 暂时失败应标记为可重试。
    EXPECT_TRUE(rgbt::gateway::IsRetryable(response.error().code()));
}

TEST_F(GatewayServiceTest, InvalidCredentialIsCheckedBeforeDependency) {
    // 凭据错误时不应因为依赖不可用而返回 503：鉴权先于依赖访问。
    sessions_.unavailable = true;
    LoginRequest request = MakeValidLogin("alice", "wrong_password");
    LoginResponse response;
    service_->Login(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 401);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().code(), ErrorCode::UNAUTHENTICATED);
}

// ---------------------------------------------------------------------------
// 查询当前玩家
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, GetCurrentPlayerSucceedsWithValidToken) {
    LoginRequest login = MakeValidLogin();
    LoginResponse login_response;
    service_->Login(nullptr, &login, &login_response, nullptr);
    ASSERT_EQ(login_response.status_code(), 200);

    GetCurrentPlayerRequest request;
    request.set_token(login_response.token());
    GetCurrentPlayerResponse response;
    service_->GetCurrentPlayer(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200);
    EXPECT_FALSE(response.has_error());
    EXPECT_EQ(response.player().player_id(), "p-0001");
}

TEST_F(GatewayServiceTest, GetCurrentPlayerRejectsEmptyToken) {
    GetCurrentPlayerRequest request;
    request.set_token("");
    GetCurrentPlayerResponse response;
    service_->GetCurrentPlayer(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().reason(), "token_required");
}

TEST_F(GatewayServiceTest, GetCurrentPlayerRejectsMalformedToken) {
    GetCurrentPlayerRequest request;
    // 含非法字符，应在访问存储之前就被拒绝。
    request.set_token("not a valid token!");
    GetCurrentPlayerResponse response;
    service_->GetCurrentPlayer(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().reason(), "token_malformed");
}

TEST_F(GatewayServiceTest, GetCurrentPlayerRejectsUnknownToken) {
    GetCurrentPlayerRequest request;
    request.set_token(rgbt::common::GenerateToken());
    GetCurrentPlayerResponse response;
    service_->GetCurrentPlayer(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 401);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().reason(), "session_not_found");
}

TEST_F(GatewayServiceTest, GetCurrentPlayerReturnsUnavailableWhenStoreFails) {
    sessions_.unavailable = true;
    GetCurrentPlayerRequest request;
    request.set_token(rgbt::common::GenerateToken());
    GetCurrentPlayerResponse response;
    service_->GetCurrentPlayer(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().code(), ErrorCode::UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// 登出
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, LogoutInvalidatesSession) {
    LoginRequest login = MakeValidLogin();
    LoginResponse login_response;
    service_->Login(nullptr, &login, &login_response, nullptr);
    ASSERT_EQ(login_response.status_code(), 200);

    LogoutRequest logout;
    logout.set_token(login_response.token());
    LogoutResponse logout_response;
    service_->Logout(nullptr, &logout, &logout_response, nullptr);
    EXPECT_EQ(logout_response.status_code(), 200);

    // 登出后原 Token 不再可用。
    GetCurrentPlayerRequest me;
    me.set_token(login_response.token());
    GetCurrentPlayerResponse me_response;
    service_->GetCurrentPlayer(nullptr, &me, &me_response, nullptr);
    EXPECT_EQ(me_response.status_code(), 401);
}

TEST_F(GatewayServiceTest, LogoutIsIdempotentForUnknownToken) {
    LogoutRequest logout;
    logout.set_token(rgbt::common::GenerateToken());
    LogoutResponse response;
    service_->Logout(nullptr, &logout, &response, nullptr);

    // 重复登出不应报错。
    EXPECT_EQ(response.status_code(), 200);
}

TEST_F(GatewayServiceTest, LogoutReturnsUnavailableWhenStoreFails) {
    sessions_.unavailable = true;
    LogoutRequest logout;
    logout.set_token(rgbt::common::GenerateToken());
    LogoutResponse response;
    service_->Logout(nullptr, &logout, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    ASSERT_TRUE(response.has_error());
    EXPECT_EQ(response.error().code(), ErrorCode::UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// 错误码与 Token 的基础行为
// ---------------------------------------------------------------------------

TEST(ErrorCodeMappingTest, MapsToExpectedHttpStatus) {
    EXPECT_EQ(rgbt::gateway::HttpStatusOf(ErrorCode::INVALID_ARGUMENT), 400);
    EXPECT_EQ(rgbt::gateway::HttpStatusOf(ErrorCode::UNAUTHENTICATED), 401);
    EXPECT_EQ(rgbt::gateway::HttpStatusOf(ErrorCode::NOT_FOUND), 404);
    EXPECT_EQ(rgbt::gateway::HttpStatusOf(ErrorCode::UNAVAILABLE), 503);
    EXPECT_EQ(rgbt::gateway::HttpStatusOf(ErrorCode::INTERNAL), 500);
    // 未指定错误码不能落成 200。
    EXPECT_EQ(rgbt::gateway::HttpStatusOf(ErrorCode::ERROR_CODE_UNSPECIFIED), 500);
}

TEST(ErrorCodeMappingTest, OnlyUnavailableIsRetryable) {
    EXPECT_TRUE(rgbt::gateway::IsRetryable(ErrorCode::UNAVAILABLE));
    EXPECT_FALSE(rgbt::gateway::IsRetryable(ErrorCode::INVALID_ARGUMENT));
    EXPECT_FALSE(rgbt::gateway::IsRetryable(ErrorCode::UNAUTHENTICATED));
}

TEST(TokenTest, GeneratesUniqueAndWellFormedTokens) {
    const std::string first = rgbt::common::GenerateToken();
    const std::string second = rgbt::common::GenerateToken();

    EXPECT_EQ(first.size(), rgbt::common::kDefaultTokenLength);
    EXPECT_NE(first, second);
    EXPECT_TRUE(rgbt::common::IsValidTokenFormat(first));
}

TEST(TokenTest, RejectsMalformedTokens) {
    EXPECT_FALSE(rgbt::common::IsValidTokenFormat(""));
    EXPECT_FALSE(rgbt::common::IsValidTokenFormat("has space"));
    EXPECT_FALSE(rgbt::common::IsValidTokenFormat("has/slash"));
    EXPECT_FALSE(rgbt::common::IsValidTokenFormat(std::string(257, 'a')));
}

}  // namespace
