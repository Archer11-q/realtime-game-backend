/// @file gateway_service_test.cpp
/// @brief Gateway 登录切片的单元测试。
///
/// 用内存假存储注入 SessionStore，因此这些用例不需要 Redis 即可运行，
/// 并且可以精确模拟「Redis 不可用」这类难以在集成环境稳定复现的路径。

#include "gateway_service.hpp"

#include <brpc/controller.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// ---------------------------------------------------------------------------
// TASK-021：每个 HTTP 请求都必须留下一条可按 trace 检索的 request_done
// ---------------------------------------------------------------------------

namespace {

/// 把 stderr 重定向到一个临时文件，析构时读回内容并恢复。
///
/// 为什么必须捕获进程级 stderr 而不是"断言格式化结果"：本任务的全部价值在于
/// 「**每个**请求都留下记录」。这一点只有在真的发出一个请求、再去看输出里
/// 有没有那一行时才算被验证；直接断言 `FormatLogLine` 只能证明格式化函数没问题，
/// 证明不了"收敛点确实被调用了"。
///
/// 为什么用 `setvbuf(_IOFBF, ...)`：把 stderr 切成全缓冲后，产品代码的输出会先落在
/// 我们提供的数组里。析构时先恢复文件描述符与缓冲模式（此时 libc 会把缓冲区刷进
/// 临时文件），再读文件。这样既不依赖任何 libc 内部细节，也不需要在多线程下工作
/// （单元测试是单线程顺序执行的）。
class CapturedStderr {
public:
    CapturedStderr() {
        const char* tmp = std::getenv("TMPDIR");
        path_ = std::string(tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp") +
                "/rgbt-gateway-request-done-" + std::to_string(::getpid()) + ".log";
        file_ = std::fopen(path_.c_str(), "w+");
        if (file_ == nullptr) {
            return;
        }
        saved_fd_ = ::dup(fileno(stderr));
        if (saved_fd_ < 0) {
            std::fclose(file_);
            file_ = nullptr;
            return;
        }
        std::fflush(stderr);
        std::setvbuf(stderr, buffer_.data(), _IOFBF, buffer_.size());
        ::dup2(fileno(file_), fileno(stderr));
    }

    CapturedStderr(const CapturedStderr&) = delete;
    CapturedStderr& operator=(const CapturedStderr&) = delete;

    ~CapturedStderr() { Stop(); }

    /// 停止捕获并返回捕获到的全部文本。可重复调用（第二次返回同一份内容）。
    std::string Stop() {
        if (saved_fd_ < 0) {
            return text_;
        }
        std::fflush(stderr);
        ::dup2(saved_fd_, fileno(stderr));
        ::close(saved_fd_);
        saved_fd_ = -1;
        std::setvbuf(stderr, nullptr, _IONBF, 0);

        std::fflush(file_);
        std::fseek(file_, 0, SEEK_END);
        const long size = std::ftell(file_);
        if (size > 0) {
            text_.resize(static_cast<std::size_t>(size));
            std::fseek(file_, 0, SEEK_SET);
            const std::size_t read = std::fread(text_.data(), 1, text_.size(), file_);
            text_.resize(read);
        }
        std::fclose(file_);
        file_ = nullptr;
        std::remove(path_.c_str());
        return text_;
    }

private:
    std::string path_;
    std::FILE* file_ = nullptr;
    int saved_fd_ = -1;
    std::array<char, 1 << 16> buffer_{};
    std::string text_;
};

/// 在文本里找同时含 `needle_a` 与 `needle_b` 的行。找不到返回空串。
std::string FindLineWith(const std::string& text, std::string_view needle_a,
                         std::string_view needle_b) {
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string line =
            text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (line.find(needle_a) != std::string::npos && line.find(needle_b) != std::string::npos) {
            return line;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return {};
}

/// `request_done` 出现次数。
std::size_t CountRequestDone(const std::string& text) {
    std::size_t count = 0;
    std::size_t pos = text.find("event=request_done");
    while (pos != std::string::npos) {
        ++count;
        pos = text.find("event=request_done", pos + 1);
    }
    return count;
}

}  // namespace

/// 补齐所有映射路径都要留下 request_done，且 op 与 restful 映射一一对应。
///
/// 为什么逐条覆盖全部 11 个路径而不是抽查两个：本任务修的是"15 个接口里只有 2 个
/// 有日志"这个**覆盖面**问题。抽查通不过只能证明抽查的那两条在，证明不了别的。
TEST_F(GatewayServiceTest, EveryMappedPathEmitsRequestDoneWithItsOperation) {
    const std::string token = LoginAlice();

    using Handler = std::int32_t (*)(GatewayServiceImpl&, ::google::protobuf::RpcController*,
                                     const std::string&, const std::string&);
    struct Case {
        const char* path;
        const char* op;
        Handler invoke;
    };

    const std::vector<Case> cases = {
        {"/api/v1/login", "login",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string&, const std::string& trace) -> std::int32_t {
             LoginRequest request;
             request.set_request_id(trace);
             LoginResponse response;
             service.Login(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/players/me", "get_current_player",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             GetCurrentPlayerRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             GetCurrentPlayerResponse response;
             service.GetCurrentPlayer(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/logout", "logout",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             LogoutRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             LogoutResponse response;
             service.Logout(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/matches", "enqueue_match",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             EnqueueMatchRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             EnqueueMatchResponse response;
             service.EnqueueMatch(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/matches/current", "get_match_status",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             GetMatchStatusRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             GetMatchStatusResponse response;
             service.GetMatchStatus(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/matches/current/cancel", "cancel_match",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             CancelMatchRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             CancelMatchResponse response;
             service.CancelMatch(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/rooms/join", "join_room",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             JoinRoomRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             request.set_room_id("r-1");
             JoinRoomResponse response;
             service.JoinRoom(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/rooms/input", "submit_input",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             SubmitInputRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             request.set_room_id("r-1");
             SubmitInputResponse response;
             service.SubmitInput(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/rooms/state", "get_room_state",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             GetRoomStateRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             request.set_room_id("r-1");
             GetRoomStateResponse response;
             service.GetRoomState(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/results", "get_match_result",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             GetMatchResultRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             request.set_match_id("m-1");
             GetMatchResultResponse response;
             service.GetMatchResult(controller, &request, &response, nullptr);
             return response.status_code();
         }},
        {"/api/v1/stream", "stream_events",
         [](GatewayServiceImpl& service, ::google::protobuf::RpcController* controller,
            const std::string& token, const std::string& trace) -> std::int32_t {
             StreamEventsRequest request;
             request.set_token(token);
             request.set_request_id(trace);
             request.set_room_id("r-1");
             StreamEventsResponse response;
             service.StreamEvents(controller, &request, &response, nullptr);
             return response.status_code();
         }},
    };

    for (const Case& item : cases) {
        const std::string trace = std::string("trace-") + item.op;
        brpc::Controller controller;
        controller.http_request().uri().set_path(item.path);
        controller.http_request().set_method(brpc::HTTP_METHOD_POST);
        controller.http_request().SetHeader("Content-Type", "application/json");

        CapturedStderr captured;
        const std::int32_t status_code = item.invoke(*service_, &controller, token, trace);
        const std::string line =
            FindLineWith(captured.Stop(), "event=request_done", std::string("trace=") + trace);
        // 这一条断言是本次任务的核心：请求结束时**一定**有一行记录，且 op 正确。
        EXPECT_FALSE(line.empty()) << "路径 " << item.path << " 没有留下 request_done";
        EXPECT_NE(line.find(std::string("op=") + item.op), std::string::npos)
            << "路径 " << item.path << " 的 op 不是 " << item.op << "：实际行 = " << line;
        // 状态码必须写进这一行，且与实际响应一致：否则"失败了但显示 200"
        // 这种表现在日志里完全看不出来。这里不写死具体数字——
        // `players/me` 与 `logout` 带合法 Token 时会成功（200），
        // 断言 4xx 会把正确行为判成失败（第一版就是这么写错的）。
        EXPECT_NE(line.find("status=" + std::to_string(status_code)), std::string::npos)
            << "路径 " << item.path << " 的 request_done 状态码与响应不一致（响应 " << status_code
            << "）：实际行 = " << line;
    }
}

/// trace 字段的完整语义：给了就带上，没给就**不编造**。
TEST_F(GatewayServiceTest, RequestDoneCarriesTheCallersTraceAndNeverFabricatesOne) {
    const std::string token = LoginAlice();

    brpc::Controller controller;
    controller.http_request().uri().set_path("/api/v1/matches");
    controller.http_request().set_method(brpc::HTTP_METHOD_POST);

    EnqueueMatchRequest with_trace;
    with_trace.set_token(token);
    with_trace.set_request_id("trace-explicit-021");
    EnqueueMatchResponse response;
    CapturedStderr first;
    service_->EnqueueMatch(&controller, &with_trace, &response, nullptr);
    const std::string capture = first.Stop();

    // 这一次请求留下两条记录：业务日志（match_enqueue_ok）与收敛点的 request_done，
    // 两者带**同一个** trace —— 这正是"一条链能串起来"的字面含义。
    const std::string business_line =
        FindLineWith(capture, "event=match_enqueue_ok", "trace=trace-explicit-021");
    const std::string done_line =
        FindLineWith(capture, "event=request_done", "trace=trace-explicit-021");
    EXPECT_FALSE(business_line.empty()) << "业务日志没带上 trace：" << capture;
    EXPECT_FALSE(done_line.empty()) << "request_done 没带上调用方的 trace：" << capture;
    // 一次请求只留一条 request_done：多写一处会让"按 trace 数请求数"这个用法失真。
    EXPECT_EQ(CountRequestDone(capture), 1U) << "一次请求应只留一条 request_done";

    EnqueueMatchRequest without_trace;
    without_trace.set_token(token);
    EnqueueMatchResponse response2;
    CapturedStderr second;
    service_->EnqueueMatch(&controller, &without_trace, &response2, nullptr);
    const std::string capture2 = second.Stop();
    // 没有 request_id 时**不输出** trace=，而不是输出 trace=- 或生成一个伪 id。
    // 判据钉在 `event=request_done` 之后紧跟的字段上：结构化行里它是紧随 event 的
    // 恒定字段，因此这个子串一旦出现就说明真的写了空的 trace。
    EXPECT_EQ(capture2.find("event=request_done trace="), std::string::npos)
        << "空 request_id 时不应输出 trace 字段：捕获内容 = " << capture2;
    EXPECT_NE(capture2.find("event=request_done"), std::string::npos)
        << "即便没有 trace，request_done 也必须留下";
}

/// 失败请求必须在级别上可分辨：`grep 'level=error'` 要能一刀切出 5xx。
TEST_F(GatewayServiceTest, RequestDoneLevelFollowsTheHttpStatus) {
    // 500：没有 HTTP 上下文的 SSE 订阅（见 StreamEventsWithoutHttpContextReturns500）。
    const std::string token = LoginAlice();
    room_.snapshot.players.push_back(RoomPlayerSnapshot{"p-0001", 100, true});
    StreamHub hub(&room_);
    GatewayServiceImpl service(&sessions_, players_.get(), &match_, &room_, &hub);

    StreamEventsRequest request;
    request.set_token(token);
    request.set_room_id("r-1");
    request.set_request_id("trace-500");
    StreamEventsResponse response;
    CapturedStderr captured;
    service.StreamEvents(nullptr, &request, &response, nullptr);
    ASSERT_EQ(response.status_code(), 500);

    const std::string line = FindLineWith(captured.Stop(), "event=request_done", "status=500");
    EXPECT_FALSE(line.empty()) << "500 响应没有留下 request_done";
    EXPECT_NE(line.find("level=error"), std::string::npos) << "5xx 应为 level=error：" << line;
    EXPECT_NE(line.find("trace=trace-500"), std::string::npos)
        << "失败路径同样要能按 trace 定位：" << line;
    // 无 HTTP 上下文时推导不出 op，但**不能编造**一个：unknown 本身就是线索。
    EXPECT_NE(line.find("op=unknown"), std::string::npos) << "无路径时应记 op=unknown：" << line;

    // 4xx：未认证的匹配入队。
    brpc::Controller controller;
    controller.http_request().uri().set_path("/api/v1/matches");
    controller.http_request().set_method(brpc::HTTP_METHOD_POST);
    EnqueueMatchRequest bad;
    bad.set_request_id("trace-400");
    EnqueueMatchResponse bad_response;
    CapturedStderr warn_captured;
    service_->EnqueueMatch(&controller, &bad, &bad_response, nullptr);
    ASSERT_EQ(bad_response.status_code(), 400);
    const std::string warn_line =
        FindLineWith(warn_captured.Stop(), "event=request_done", "status=400");
    EXPECT_FALSE(warn_line.empty()) << "400 响应没有留下 request_done";
    EXPECT_NE(warn_line.find("level=warn"), std::string::npos)
        << "4xx 应为 level=warn：" << warn_line;
    EXPECT_NE(warn_line.find("trace=trace-400"), std::string::npos)
        << "被拒绝的请求也要能按 trace 定位：" << warn_line;
}

/// 未被 restful 映射的路径必须记成 `op=unknown`，而不是猜一个看起来合理的动作名。
TEST_F(GatewayServiceTest, UnmappedPathIsRecordedAsUnknownOperation) {
    const std::string token = LoginAlice();
    brpc::Controller controller;
    controller.http_request().uri().set_path("/status");
    controller.http_request().set_method(brpc::HTTP_METHOD_GET);

    GetCurrentPlayerRequest request;
    request.set_token(token);
    GetCurrentPlayerResponse response;
    CapturedStderr captured;
    service_->GetCurrentPlayer(&controller, &request, &response, nullptr);
    const std::string capture = captured.Stop();

    EXPECT_NE(capture.find("event=request_done"), std::string::npos);
    EXPECT_NE(capture.find("op=unknown"), std::string::npos)
        << "非映射路径必须记 op=unknown，不能猜一个动作名：" << capture;
    EXPECT_EQ(capture.find("op=get_current_player"), std::string::npos)
        << "op 必须由实际路径推导，不能来自处理函数名：" << capture;
}

}  // namespace
