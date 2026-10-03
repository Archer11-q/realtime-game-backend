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
#include "room_client.hpp"
#include "session_store.hpp"
#include "stream_hub.hpp"

namespace {

using rgbt::gateway::CredentialStatus;
using rgbt::gateway::GatewayServiceImpl;
using rgbt::gateway::MatchCallStatus;
using rgbt::gateway::MatchClient;
using rgbt::gateway::MatchResultView;
using rgbt::gateway::MatchSnapshot;
using rgbt::gateway::MatchState;
using rgbt::gateway::PlayerDirectory;
using rgbt::gateway::RoomCallStatus;
using rgbt::gateway::RoomClient;
using rgbt::gateway::RoomPlayerSnapshot;
using rgbt::gateway::RoomSnapshot;
using rgbt::gateway::RoomState;
using rgbt::gateway::SessionRecord;
using rgbt::gateway::SessionStore;
using rgbt::gateway::StoreStatus;
using rgbt::gateway::StreamHub;
using rgbt::gateway::v1::CancelMatchRequest;
using rgbt::gateway::v1::CancelMatchResponse;
using rgbt::gateway::v1::EnqueueMatchRequest;
using rgbt::gateway::v1::EnqueueMatchResponse;
using rgbt::gateway::v1::ErrorCode;
using rgbt::gateway::v1::GetCurrentPlayerRequest;
using rgbt::gateway::v1::GetCurrentPlayerResponse;
using rgbt::gateway::v1::GetMatchResultRequest;
using rgbt::gateway::v1::GetMatchResultResponse;
using rgbt::gateway::v1::GetMatchStatusRequest;
using rgbt::gateway::v1::GetMatchStatusResponse;
using rgbt::gateway::v1::GetRoomStateRequest;
using rgbt::gateway::v1::GetRoomStateResponse;
using rgbt::gateway::v1::JoinRoomRequest;
using rgbt::gateway::v1::JoinRoomResponse;
using rgbt::gateway::v1::LoginRequest;
using rgbt::gateway::v1::LoginResponse;
using rgbt::gateway::v1::LogoutRequest;
using rgbt::gateway::v1::LogoutResponse;
using rgbt::gateway::v1::StreamEventsRequest;
using rgbt::gateway::v1::StreamEventsResponse;
using rgbt::gateway::v1::SubmitInputRequest;
using rgbt::gateway::v1::SubmitInputResponse;

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

/// 内存房间客户端。
///
/// 与 FakeMatchClient 同样的目的：让「Room 不可用」「对局已结束」「结果尚未落库」
/// 这些依赖错误可以在单元测试里稳定复现，而不必停掉一个进程或制造 MySQL 故障。
class FakeRoomClient : public RoomClient {
public:
    RoomCallStatus next_status = RoomCallStatus::kOk;
    RoomSnapshot snapshot;

    /// GetResult 返回的视图。
    MatchResultView result_view;

    /// 记录最近一次调用传入的参数，用于验证「player_id 来自会话」「room_id 被透传」。
    std::string last_room_id;
    std::string last_player_id;
    std::string last_match_id;
    int join_calls = 0;
    int submit_calls = 0;
    int state_calls = 0;
    int result_calls = 0;

    RoomCallStatus Join(const std::string& room_id, const std::string& player_id,
                        const std::string& /*request_id*/, RoomSnapshot* out_snapshot) override {
        ++join_calls;
        last_room_id = room_id;
        last_player_id = player_id;
        if (out_snapshot != nullptr) {
            *out_snapshot = snapshot;
        }
        return next_status;
    }

    RoomCallStatus SubmitAttack(const std::string& room_id, const std::string& player_id,
                                const std::string& /*request_id*/,
                                RoomSnapshot* out_snapshot) override {
        ++submit_calls;
        last_room_id = room_id;
        last_player_id = player_id;
        if (out_snapshot != nullptr) {
            *out_snapshot = snapshot;
        }
        return next_status;
    }

    RoomCallStatus GetState(const std::string& room_id, const std::string& /*request_id*/,
                            RoomSnapshot* out_snapshot) override {
        ++state_calls;
        last_room_id = room_id;
        if (out_snapshot != nullptr) {
            *out_snapshot = snapshot;
        }
        return next_status;
    }

    RoomCallStatus GetResult(const std::string& match_id, const std::string& /*request_id*/,
                             MatchResultView* out_view) override {
        ++result_calls;
        last_match_id = match_id;
        if (out_view != nullptr) {
            *out_view = result_view;
        }
        return next_status;
    }

    /// TASK-017：本用例集不验证补发（那是 stream_hub_test 的职责），
    /// 因此只做最小实现，但**必须实现**——抽象接口不会给默认行为。
    RoomCallStatus GetSnapshotsSince(const std::string&, std::int64_t, const std::string&,
                                     rgbt::gateway::SnapshotRange*) override {
        return RoomCallStatus::kOk;
    }

    /// TASK-016：本用例集不验证 presence 上报（那是 stream_hub_test 的职责），
    /// 因此这里只做最小实现，但**必须实现**——抽象接口不会给默认行为，
    /// 这正是"新增 RPC 会强迫所有调用方表态"的体现。
    RoomCallStatus SetPresence(const std::string&, const std::string&, bool, const std::string&,
                               RoomSnapshot*) override {
        return RoomCallStatus::kOk;
    }

    [[nodiscard]] bool IsHealthy() override { return true; }
};

/// 测试夹具：构造服务与依赖。
class GatewayServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // PlayerDirectory、MatchClient 与 RoomClient 都是接口，需要以指针持有。
        // 三者都使用不访问外部依赖的实现，因此这些用例不需要 Redis/MySQL/Match/Room。
        players_ = PlayerDirectory::WithBuiltinTestAccounts();
        service_ =
            std::make_unique<GatewayServiceImpl>(&sessions_, players_.get(), &match_, &room_);
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
    FakeRoomClient room_;
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

// ---------------------------------------------------------------------------
// 房间与对局结果（TASK-008）
// ---------------------------------------------------------------------------

/// 构造一个双方都在房间内、处于 PLAYING 的快照。
RoomSnapshot PlayingSnapshot() {
    RoomSnapshot snapshot;
    snapshot.room_id = "r-1";
    snapshot.match_id = "m-1";
    snapshot.state = RoomState::kPlaying;
    snapshot.frame = 3;
    snapshot.started_at_ms = 1000;

    RoomPlayerSnapshot first;
    first.player_id = "p-0001";
    first.hp = 100;
    first.connected = true;
    RoomPlayerSnapshot second;
    second.player_id = "p-0002";
    second.hp = 70;
    second.connected = true;
    snapshot.players = {first, second};
    return snapshot;
}

TEST_F(GatewayServiceTest, JoinRoomRequiresToken) {
    JoinRoomRequest request;
    request.set_room_id("r-1");
    JoinRoomResponse response;
    service_->JoinRoom(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "token_required");
    EXPECT_EQ(room_.join_calls, 0);
}

TEST_F(GatewayServiceTest, JoinRoomRequiresRoomId) {
    const std::string token = LoginAlice();

    JoinRoomRequest request;
    request.set_token(token);
    JoinRoomResponse response;
    service_->JoinRoom(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "room_id_required");
    EXPECT_EQ(room_.join_calls, 0);
}

TEST_F(GatewayServiceTest, JoinRoomUsesPlayerIdFromSession) {
    const std::string token = LoginAlice();
    room_.snapshot = PlayingSnapshot();

    JoinRoomRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    JoinRoomResponse response;
    service_->JoinRoom(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200) << response.error().message();
    // player_id 必须来自会话，绝不能来自请求体——否则任何登录用户都能替别人进房。
    EXPECT_EQ(room_.last_player_id, "p-0001");
    EXPECT_EQ(room_.last_room_id, "r-1");
    EXPECT_EQ(response.room().state(), "playing");
    ASSERT_EQ(response.room().players_size(), 2);
    EXPECT_EQ(response.room().players(1).hp(), 70);
}

TEST_F(GatewayServiceTest, JoinRoomUnavailableReturns503) {
    const std::string token = LoginAlice();
    room_.next_status = RoomCallStatus::kUnavailable;

    JoinRoomRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    JoinRoomResponse response;
    service_->JoinRoom(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "room_unavailable");
}

TEST_F(GatewayServiceTest, JoinRoomNotFoundReturns404) {
    const std::string token = LoginAlice();
    room_.next_status = RoomCallStatus::kNotFound;

    JoinRoomRequest request;
    request.set_token(token);
    request.set_room_id("r-gone");
    JoinRoomResponse response;
    service_->JoinRoom(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 404);
    EXPECT_EQ(response.error().reason(), "room_not_found");
}

TEST_F(GatewayServiceTest, SubmitInputUsesPlayerIdFromSession) {
    const std::string token = LoginAlice();
    room_.snapshot = PlayingSnapshot();

    SubmitInputRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    SubmitInputResponse response;
    service_->SubmitInput(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200) << response.error().message();
    EXPECT_EQ(room_.last_player_id, "p-0001");
    EXPECT_EQ(room_.submit_calls, 1);
}

TEST_F(GatewayServiceTest, SubmitInputAfterFinishReturns409) {
    const std::string token = LoginAlice();
    room_.next_status = RoomCallStatus::kAlreadyFinished;

    SubmitInputRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    SubmitInputResponse response;
    service_->SubmitInput(nullptr, &request, &response, nullptr);

    // 409 而不是 400：请求格式没问题，是当前状态不允许这个操作。
    EXPECT_EQ(response.status_code(), 409);
    EXPECT_EQ(response.error().reason(), "room_already_finished");
}

TEST_F(GatewayServiceTest, GetRoomStateReturnsSnapshot) {
    const std::string token = LoginAlice();
    room_.snapshot = PlayingSnapshot();

    GetRoomStateRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    GetRoomStateResponse response;
    service_->GetRoomState(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200) << response.error().message();
    EXPECT_EQ(response.room().room_id(), "r-1");
    EXPECT_EQ(response.room().match_id(), "m-1");
    EXPECT_EQ(response.room().frame(), 3);
}

TEST_F(GatewayServiceTest, GetMatchResultReturnsResult) {
    const std::string token = LoginAlice();
    room_.result_view.has_result = true;
    room_.result_view.result.match_id = "m-1";
    room_.result_view.result.room_id = "r-1";
    room_.result_view.result.winner_id = "p-0001";
    room_.result_view.result.player_count = 2;
    room_.result_view.result.started_at_ms = 1000;
    room_.result_view.result.finished_at_ms = 61000;

    GetMatchResultRequest request;
    request.set_token(token);
    request.set_match_id("m-1");
    GetMatchResultResponse response;
    service_->GetMatchResult(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 200) << response.error().message();
    EXPECT_EQ(response.result().winner_id(), "p-0001");
    EXPECT_EQ(response.result().player_count(), 2);
}

TEST_F(GatewayServiceTest, GetMatchResultPendingReturns503Not404) {
    const std::string token = LoginAlice();
    room_.next_status = RoomCallStatus::kResultPending;
    room_.result_view.has_room = true;
    room_.result_view.room = PlayingSnapshot();

    GetMatchResultRequest request;
    request.set_token(token);
    request.set_match_id("m-1");
    GetMatchResultResponse response;
    service_->GetMatchResult(nullptr, &request, &response, nullptr);

    // 结果**存在**，只是还没落库。返回 404 会让客户端以为这局没有结果。
    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "result_pending");
    // result 必须留空：调用方不得据此推断胜负。
    EXPECT_FALSE(response.has_result());
    // 但房间快照要给出，便于判断对局是否已结束。
    EXPECT_TRUE(response.has_room());
}

TEST_F(GatewayServiceTest, GetMatchResultStoreDownReturnsStoreReason) {
    const std::string token = LoginAlice();
    room_.next_status = RoomCallStatus::kStoreUnavailable;

    GetMatchResultRequest request;
    request.set_token(token);
    request.set_match_id("m-1");
    GetMatchResultResponse response;
    service_->GetMatchResult(nullptr, &request, &response, nullptr);

    // 与 room_unavailable 分开：排障时「哪个依赖挂了」是第一个要回答的问题。
    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "result_store_unavailable");
}

TEST_F(GatewayServiceTest, RoomEndpointsReturn503WhenSessionStoreIsDown) {
    const std::string token = LoginAlice();
    sessions_.unavailable = true;

    // 会话存储不可用时，鉴权阶段就应该失败，且不应触碰 Room。
    GetRoomStateRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    GetRoomStateResponse response;
    service_->GetRoomState(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "session_store_unavailable");
    EXPECT_EQ(room_.state_calls, 0);
}

// ---------------------------------------------------------------------------
// 服务端推送（SSE，TASK-009）
//
// 成功路径（真的建立 text/event-stream 连接）无法在这里验证：它需要 brpc 的
// HTTP 请求上下文才能拿到 ProgressiveAttachment，而单元测试直接调用服务方法。
// 因此这里覆盖全部**失败路径**，成功路径由 scripts/verify-stream.sh 端到端覆盖。
// ---------------------------------------------------------------------------

TEST_F(GatewayServiceTest, StreamEventsRequiresToken) {
    StreamEventsRequest request;
    request.set_room_id("r-1");
    StreamEventsResponse response;
    service_->StreamEvents(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "token_required");
    // 鉴权不过就不应触碰 Room。
    EXPECT_EQ(room_.state_calls, 0);
}

TEST_F(GatewayServiceTest, StreamEventsRequiresRoomId) {
    const std::string token = LoginAlice();
    StreamEventsRequest request;
    request.set_token(token);
    StreamEventsResponse response;
    service_->StreamEvents(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "room_id_required");
    EXPECT_EQ(room_.state_calls, 0);
}

TEST_F(GatewayServiceTest, StreamEventsWithoutHubReturns503) {
    // 夹具默认不注入 StreamHub（见 SetUp），因此推送接口应明确报不可用，
    // 而不是静默建立一条永远收不到事件的流。
    const std::string token = LoginAlice();
    room_.snapshot.players.push_back(RoomPlayerSnapshot{"p-0001", 100, true});

    StreamEventsRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    StreamEventsResponse response;
    service_->StreamEvents(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 503);
    EXPECT_EQ(response.error().reason(), "stream_unavailable");
}

TEST_F(GatewayServiceTest, StreamEventsRejectsNonMember) {
    // 核心访问控制：房间存在，但调用者不在名单里。
    // 没有这条校验，任何登录用户只要拿到 room_id 就能长期订阅别人的血量。
    const std::string token = LoginAlice();
    room_.snapshot.players.push_back(RoomPlayerSnapshot{"p-0002", 100, true});

    StreamEventsRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    StreamEventsResponse response;
    service_->StreamEvents(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 400);
    EXPECT_EQ(response.error().reason(), "not_a_member");
    // 必须真的查过房间才可能知道谁是成员。
    EXPECT_EQ(room_.state_calls, 1);
}

TEST_F(GatewayServiceTest, StreamEventsPropagatesRoomNotFound) {
    const std::string token = LoginAlice();
    room_.next_status = RoomCallStatus::kNotFound;

    StreamEventsRequest request;
    request.set_token(token);
    request.set_room_id("r-missing");
    StreamEventsResponse response;
    service_->StreamEvents(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 404);
    EXPECT_EQ(response.error().reason(), "room_not_found");
}

TEST_F(GatewayServiceTest, StreamEventsWithoutHttpContextReturns500) {
    // 注入了 StreamHub 且成员校验通过，但没有 HTTP 上下文——
    // 此时不能"假装成功"：必须明确失败，否则会登记一条永远写不出去的订阅。
    const std::string token = LoginAlice();
    room_.snapshot.players.push_back(RoomPlayerSnapshot{"p-0001", 100, true});
    StreamHub hub(&room_);
    GatewayServiceImpl service(&sessions_, players_.get(), &match_, &room_, &hub);

    StreamEventsRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    StreamEventsResponse response;
    service.StreamEvents(nullptr, &request, &response, nullptr);

    EXPECT_EQ(response.status_code(), 500);
    EXPECT_EQ(response.error().reason(), "stream_requires_http");
    // 关键：没有登记任何订阅。登记了就会给同一个房间凭空增加轮询。
    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_EQ(hub.RoomPollCount(), 0U);
}

}  // namespace
