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
#include "match_client.hpp"
#include "player_directory.hpp"
#include "session_store.hpp"

namespace {

using rgbt::gateway::CredentialStatus;
using rgbt::gateway::GatewayServiceImpl;
using rgbt::gateway::MatchCallStatus;
using rgbt::gateway::MatchClient;
using rgbt::gateway::MatchSnapshot;
using rgbt::gateway::MatchState;
using rgbt::gateway::PlayerDirectory;
using rgbt::gateway::SessionRecord;
using rgbt::gateway::SessionStore;
using rgbt::gateway::StoreStatus;
using rgbt::gateway::v1::CancelMatchRequest;
using rgbt::gateway::v1::CancelMatchResponse;
using rgbt::gateway::v1::EnqueueMatchRequest;
using rgbt::gateway::v1::EnqueueMatchResponse;
using rgbt::gateway::v1::ErrorCode;
using rgbt::gateway::v1::GetCurrentPlayerRequest;
using rgbt::gateway::v1::GetCurrentPlayerResponse;
using rgbt::gateway::v1::GetMatchStatusRequest;
using rgbt::gateway::v1::GetMatchStatusResponse;
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

/// 内存匹配客户端。
///
/// 存在的意义是让「Match 不可用」「队列已满」这类依赖错误可以在单元测试里稳定
/// 复现——这些路径在集成环境里要靠停掉一个进程才能造出来。
class FakeMatchClient : public MatchClient {
public:
    /// 下一次（以及之后所有）调用的返回值。
    MatchCallStatus next_status = MatchCallStatus::kOk;

    /// GetStatus/Enqueue 返回的快照。
    MatchSnapshot snapshot;

    /// 记录最近一次调用传入的 player_id，用于验证「player_id 来自会话」。
    std::string last_player_id;
    std::string last_request_id;
    int enqueue_calls = 0;
    int status_calls = 0;
    int cancel_calls = 0;

    MatchCallStatus Enqueue(const std::string& player_id, const std::string& request_id,
                            MatchSnapshot* out_snapshot) override {
        ++enqueue_calls;
        last_player_id = player_id;
        last_request_id = request_id;
        if (out_snapshot != nullptr) {
            *out_snapshot = snapshot;
        }
        return next_status;
    }

    MatchCallStatus GetStatus(const std::string& player_id, MatchSnapshot* out_snapshot) override {
        ++status_calls;
        last_player_id = player_id;
        if (out_snapshot != nullptr) {
            *out_snapshot = snapshot;
        }
        return next_status;
    }

    MatchCallStatus Cancel(const std::string& player_id, const std::string& request_id,
                           MatchSnapshot* out_snapshot) override {
        ++cancel_calls;
        last_player_id = player_id;
        last_request_id = request_id;
        if (out_snapshot != nullptr) {
            *out_snapshot = snapshot;
        }
        return next_status;
    }

    [[nodiscard]] bool IsHealthy() override { return true; }
};

/// 测试夹具：构造服务与依赖。
class GatewayServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // PlayerDirectory 与 MatchClient 都是接口，需要以指针持有。
        // 两者都使用不访问外部依赖的实现，因此这些用例不需要 Redis/MySQL/Match。
        players_ = PlayerDirectory::WithBuiltinTestAccounts();
        service_ = std::make_unique<GatewayServiceImpl>(&sessions_, players_.get(), &match_);
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

    /// 登录 alice 并返回 Token，供需要已认证会话的用例使用。
    std::string LoginAlice(const std::string& request_id = "") {
        LoginRequest request = MakeValidLogin("alice", "alice_dev_pw", request_id);
        LoginResponse response;
        service_->Login(nullptr, &request, &response, nullptr);
        return response.token();
    }

    FakeSessionStore sessions_;
    FakeMatchClient match_;
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

TEST(ErrorCodeMappingTest, RetryableCoversUnavailableAndThrottling) {
    // 依赖暂时失败与限流都可以重试（限流需退避）；输入错误与未认证重试无意义。
    EXPECT_TRUE(rgbt::gateway::IsRetryable(ErrorCode::UNAVAILABLE));
    EXPECT_TRUE(rgbt::gateway::IsRetryable(ErrorCode::RESOURCE_EXHAUSTED));
    EXPECT_FALSE(rgbt::gateway::IsRetryable(ErrorCode::INVALID_ARGUMENT));
    EXPECT_FALSE(rgbt::gateway::IsRetryable(ErrorCode::UNAUTHENTICATED));
}

TEST(ErrorCodeMappingTest, ResourceExhaustedMapsToTooManyRequests) {
    // 队列已满必须是 429 而不是 503：请求本身没错，是当前容量不足。
    EXPECT_EQ(rgbt::gateway::HttpStatusOf(ErrorCode::RESOURCE_EXHAUSTED), 429);
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

// ---------------------------------------------------------------------------
// 匹配：进入队列（TASK-007）
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, EnqueueMatchRequiresToken) {
    EnqueueMatchRequest request;
    request.set_token("");
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "token_required");
    // 未通过鉴权时不应触碰 Match。
    EXPECT_EQ(match_.enqueue_calls, 0);
}

TEST_F(GatewayServiceTest, EnqueueMatchRejectsUnknownToken) {
    EnqueueMatchRequest request;
    // 格式合法但从未签发过的 Token。
    request.set_token(rgbt::common::GenerateToken());
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 401);
    EXPECT_EQ(response.error().reason(), "session_not_found");
    EXPECT_EQ(match_.enqueue_calls, 0);
}

TEST_F(GatewayServiceTest, EnqueueMatchUsesPlayerIdFromSessionNotRequest) {
    const std::string token = LoginAlice();

    match_.snapshot.state = MatchState::kQueued;
    match_.snapshot.queue_size = 1;

    EnqueueMatchRequest request;
    request.set_token(token);
    request.set_request_id("req-match-1");
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(response.match().state(), "queued");
    EXPECT_EQ(response.match().queue_size(), 1);
    // 关键安全断言：传给 Match 的 player_id 来自会话（alice = p-0001），
    // 请求体里没有这个字段，也没有任何途径让客户端指定它。
    EXPECT_EQ(match_.last_player_id, "p-0001");
    EXPECT_EQ(match_.last_request_id, "req-match-1");
}

TEST_F(GatewayServiceTest, EnqueueMatchReturnsMatchedSnapshot) {
    const std::string token = LoginAlice();

    match_.snapshot.state = MatchState::kMatched;
    match_.snapshot.match_id = "m-abc";
    match_.snapshot.room_id = "room-m-abc";
    match_.snapshot.player_ids = {"p-0001", "p-0002"};
    match_.snapshot.queued_at_ms = 1234;

    EnqueueMatchRequest request;
    request.set_token(token);
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(response.match().state(), "matched");
    EXPECT_EQ(response.match().match_id(), "m-abc");
    EXPECT_EQ(response.match().room_id(), "room-m-abc");
    ASSERT_EQ(response.match().player_ids_size(), 2);
    EXPECT_EQ(response.match().player_ids(0), "p-0001");
    EXPECT_EQ(response.match().player_ids(1), "p-0002");
    EXPECT_EQ(response.match().queued_at_ms(), 1234);
}

TEST_F(GatewayServiceTest, EnqueueMatchUnavailableReturns503) {
    const std::string token = LoginAlice();
    match_.next_status = MatchCallStatus::kUnavailable;

    EnqueueMatchRequest request;
    request.set_token(token);
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    // Match 未启动时必须返回 503 而不是 500，也不允许伪装成功。
    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "match_unavailable");
    EXPECT_TRUE(rgbt::gateway::IsRetryable(response.error().code()));
}

TEST_F(GatewayServiceTest, EnqueueMatchQueueFullReturns429) {
    const std::string token = LoginAlice();
    match_.next_status = MatchCallStatus::kQueueFull;

    EnqueueMatchRequest request;
    request.set_token(token);
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 429);
    EXPECT_EQ(response.error().reason(), "match_queue_full");
    EXPECT_EQ(response.error().code(), ErrorCode::RESOURCE_EXHAUSTED);
}

TEST_F(GatewayServiceTest, EnqueueMatchInvalidArgumentReturns400) {
    const std::string token = LoginAlice();
    match_.next_status = MatchCallStatus::kInvalidArgument;

    EnqueueMatchRequest request;
    request.set_token(token);
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "match_invalid_argument");
}

TEST_F(GatewayServiceTest, EnqueueMatchInternalReturns500) {
    const std::string token = LoginAlice();
    match_.next_status = MatchCallStatus::kInternal;

    EnqueueMatchRequest request;
    request.set_token(token);
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 500);
    EXPECT_EQ(response.error().reason(), "match_internal");
}

// ---------------------------------------------------------------------------
// 匹配：查询状态
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, GetMatchStatusRequiresToken) {
    GetMatchStatusRequest request;
    GetMatchStatusResponse response;
    service_->GetMatchStatus(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "token_required");
    EXPECT_EQ(match_.status_calls, 0);
}

TEST_F(GatewayServiceTest, GetMatchStatusReturnsIdleForNewPlayer) {
    const std::string token = LoginAlice();
    match_.snapshot.state = MatchState::kIdle;

    GetMatchStatusRequest request;
    request.set_token(token);
    GetMatchStatusResponse response;
    service_->GetMatchStatus(nullptr, &request, &response, nullptr);

    // 「刚登录还没匹配过」是正常情形，不是错误：客户端不必为它写错误分支。
    EXPECT_EQ(response.status_code(), 200);
    EXPECT_FALSE(response.has_error());
    EXPECT_EQ(response.match().state(), "idle");
    EXPECT_EQ(match_.last_player_id, "p-0001");
}

TEST_F(GatewayServiceTest, GetMatchStatusReturnsTimeoutState) {
    const std::string token = LoginAlice();
    match_.snapshot.state = MatchState::kTimeout;

    GetMatchStatusRequest request;
    request.set_token(token);
    GetMatchStatusResponse response;
    service_->GetMatchStatus(nullptr, &request, &response, nullptr);

    // 超时必须与 idle 区分：前者提示「重新匹配」，后者是「可以开始匹配」。
    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(response.match().state(), "timeout");
}

TEST_F(GatewayServiceTest, GetMatchStatusUnavailableReturns503) {
    const std::string token = LoginAlice();
    match_.next_status = MatchCallStatus::kUnavailable;

    GetMatchStatusRequest request;
    request.set_token(token);
    GetMatchStatusResponse response;
    service_->GetMatchStatus(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "match_unavailable");
}

// ---------------------------------------------------------------------------
// 匹配：取消
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, CancelMatchIsIdempotentWhenNotQueued) {
    const std::string token = LoginAlice();
    // 玩家本来就不在队列中：Match 返回空闲状态，Gateway 按幂等成功处理。
    match_.snapshot.state = MatchState::kIdle;

    CancelMatchRequest request;
    request.set_token(token);
    CancelMatchResponse response;
    service_->CancelMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200);
    EXPECT_FALSE(response.has_error());
    EXPECT_EQ(response.match().state(), "idle");
    EXPECT_EQ(match_.cancel_calls, 1);
}

TEST_F(GatewayServiceTest, CancelMatchRemovesQueuedPlayer) {
    const std::string token = LoginAlice();
    match_.snapshot.state = MatchState::kIdle;

    CancelMatchRequest request;
    request.set_token(token);
    request.set_request_id("req-cancel-1");
    CancelMatchResponse response;
    service_->CancelMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200);
    EXPECT_EQ(match_.last_player_id, "p-0001");
    EXPECT_EQ(match_.last_request_id, "req-cancel-1");
}

TEST_F(GatewayServiceTest, CancelMatchRequiresToken) {
    CancelMatchRequest request;
    CancelMatchResponse response;
    service_->CancelMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "token_required");
    EXPECT_EQ(match_.cancel_calls, 0);
}

TEST_F(GatewayServiceTest, CancelMatchUnavailableReturns503) {
    const std::string token = LoginAlice();
    match_.next_status = MatchCallStatus::kUnavailable;

    CancelMatchRequest request;
    request.set_token(token);
    CancelMatchResponse response;
    service_->CancelMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "match_unavailable");
}

TEST_F(GatewayServiceTest, MatchEndpointsReturn503WhenSessionStoreIsDown) {
    const std::string token = LoginAlice();
    sessions_.unavailable = true;

    // 会话存储不可用时，鉴权阶段就应该失败，且不应触碰 Match。
    EnqueueMatchRequest request;
    request.set_token(token);
    EnqueueMatchResponse response;
    service_->EnqueueMatch(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "session_store_unavailable");
    EXPECT_EQ(match_.enqueue_calls, 0);
}

}  // namespace
