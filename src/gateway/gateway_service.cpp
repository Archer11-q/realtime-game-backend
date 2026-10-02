#include "gateway_service.hpp"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <brpc/http_status_code.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/token.hpp"
#include "error.hpp"

namespace rgbt::gateway {
namespace {

using rgbt::gateway::v1::ErrorCode;

/// 把业务状态码同步到 HTTP 响应上。
///
/// 背景：brpc 把 protobuf 响应序列化为 JSON 作为 body，但 HTTP 状态码默认恒为
/// 200，业务状态只体现在 body 里。依据 docs/05-api-and-data.md 第 3 节，错误
/// 必须能被调用方在传输层识别，因此这里显式设置 HTTP 状态码。
///
/// 仅在 HTTP 请求上下文中有意义：通过 RPC 调用本服务时没有 HTTP 响应，
/// 此时跳过设置。
void ApplyHttpStatus(::google::protobuf::RpcController* controller, std::int32_t status_code) {
    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return;
    }
    cntl->http_response().set_status_code(status_code);
}

/// 从 HTTP 请求中取出 Token。
///
/// 为什么需要这个函数：brpc 只把 HTTP body（JSON）映射到 protobuf 字段，
/// **不会**把 HTTP 头映射进任何字段（见官方文档 http_service.md 中 headers 与
/// query string 的说明）。因此 `Authorization` 头必须由服务自己读取，否则请求中的
/// token 字段永远是空字符串，表现为「所有携带 Token 的请求都报 token_required」。
///
/// 取值顺序：
///   1. 请求体中的 token 字段（纯 RPC 调用或显式传参时使用）；
///   2. `Authorization: Bearer <token>`，HTTP 客户端的标准做法；
///   3. query string 的 `token`，便于用浏览器或 curl 直接调试。
///
/// 无 HTTP 上下文时（例如单元测试直接调用服务）只使用请求体字段。
std::string ExtractToken(::google::protobuf::RpcController* controller,
                         const std::string& body_token) {
    if (!body_token.empty()) {
        return body_token;
    }
    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return {};
    }
    const brpc::HttpHeader& header = cntl->http_request();

    if (const std::string* auth = header.GetHeader("Authorization"); auth != nullptr) {
        // 接受 "Bearer <token>" 与直接给出 token 两种形式，前缀大小写不敏感。
        constexpr std::string_view kBearer = "bearer ";
        if (auth->size() > kBearer.size()) {
            std::string prefix = auth->substr(0, kBearer.size());
            std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (prefix == kBearer) {
                return auth->substr(kBearer.size());
            }
        }
        return *auth;
    }

    if (const std::string* query_token = header.uri().GetQuery("token"); query_token != nullptr) {
        return *query_token;
    }

    return {};
}

/// 从 query string 中取一个参数。
///
/// 与 ExtractToken 同样的理由：brpc 只把 HTTP body（JSON）映射到 protobuf 字段，
/// **query string 不会被映射进任何字段**。TASK-008 的房间与结果接口把 room_id /
/// match_id 放在查询参数里（因为 brpc 的 restful 映射不支持 `{name}` 路径参数，
/// 而 `*` 通配又会与 /rooms/join 这类固定子路径冲突），因此必须在这里显式读取，
/// 否则请求里的同名字段永远是空字符串，表现为「所有查询都报 xxx_required」。
///
/// 取值顺序与 ExtractToken 一致：请求体优先（纯 RPC 调用），其次 query string。
/// 无 HTTP 上下文时（单元测试直接调用服务）只使用请求体字段。
std::string ExtractQueryParam(::google::protobuf::RpcController* controller, const char* name,
                              const std::string& body_value) {
    if (!body_value.empty()) {
        return body_value;
    }
    auto* cntl = static_cast<brpc::Controller*>(controller);
    if (cntl == nullptr || !cntl->has_http_request()) {
        return {};
    }
    if (const std::string* value = cntl->http_request().uri().GetQuery(name); value != nullptr) {
        return *value;
    }
    return {};
}

/// 校验登录请求的输入。返回空字符串表示通过，否则返回失败原因。
std::string ValidateLoginRequest(const rgbt::gateway::v1::LoginRequest& request) {
    if (request.account().empty()) {
        return "account_required";
    }
    if (request.account().size() > kMaxAccountLength) {
        return "account_too_long";
    }
    if (request.password().empty()) {
        return "password_required";
    }
    if (request.password().size() > kMaxPasswordLength) {
        return "password_too_long";
    }
    if (request.request_id().size() > kMaxRequestIdLength) {
        return "request_id_too_long";
    }
    // client_type 允许为空，视为 web；非空时只接受已知取值。
    const std::string& client_type = request.client_type();
    if (!client_type.empty() && client_type != "web" && client_type != "bot") {
        return "unsupported_client_type";
    }
    return {};
}

/// 校验 Token 格式。返回空字符串表示通过。
std::string ValidateToken(const std::string& token) {
    if (token.empty()) {
        return "token_required";
    }
    if (token.size() > kMaxTokenLength) {
        return "token_too_long";
    }
    if (!rgbt::common::IsValidTokenFormat(token)) {
        return "token_malformed";
    }
    return {};
}

/// 匹配状态 -> 对外字符串。
///
/// 对外用字符串而不是枚举数字：浏览器直接可用，且新增状态时不需要客户端同步升级
/// 枚举定义。字符串取值与 api/proto/match.proto 的枚举名一一对应。
std::string MatchStateName(MatchState state) {
    switch (state) {
        case MatchState::kIdle:
            return "idle";
        case MatchState::kQueued:
            return "queued";
        case MatchState::kMatched:
            return "matched";
        case MatchState::kTimeout:
            return "timeout";
    }
    return "idle";
}

}  // namespace

GatewayServiceImpl::GatewayServiceImpl(SessionStore* sessions, PlayerDirectory* players,
                                       MatchClient* match, RoomClient* room,
                                       std::int32_t session_ttl_seconds)
    : sessions_(sessions),
      players_(players),
      match_(match),
      room_(room),
      session_ttl_seconds_(session_ttl_seconds > 0 ? session_ttl_seconds
                                                   : kDefaultSessionTtlSeconds) {}

std::int32_t GatewayServiceImpl::FillError(rgbt::gateway::v1::Error* error,
                                           rgbt::gateway::v1::ErrorCode code,
                                           const std::string& reason, const std::string& message,
                                           const std::string& request_id) {
    if (error != nullptr) {
        error->set_code(code);
        error->set_reason(reason);
        error->set_message(message);
        error->set_request_id(request_id);
    }
    return rgbt::gateway::HttpStatusOf(code);
}

void GatewayServiceImpl::Login(::google::protobuf::RpcController* controller,
                               const rgbt::gateway::v1::LoginRequest* request,
                               rgbt::gateway::v1::LoginResponse* response,
                               ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string request_id = request->request_id();

    // 第一步：输入校验。输入错误不重试。
    const std::string input_error = ValidateLoginRequest(*request);
    if (!input_error.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, input_error,
                      "登录请求参数不合法", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    // 第二步：凭据校验。先鉴权再碰依赖，避免为无效凭据产生会话写入。
    rgbt::gateway::v1::PlayerInfo player;
    const CredentialStatus credential =
        players_->Authenticate(request->account(), request->password(), &player);
    if (credential == CredentialStatus::kInvalidCredential) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAUTHENTICATED,
                                              "invalid_credential", "账号或密码不正确", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }
    if (credential == CredentialStatus::kAccountDisabled) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAUTHENTICATED,
                                              "account_disabled", "账号已被禁用", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }
    if (credential == CredentialStatus::kUnavailable) {
        // 玩家档案来自 MySQL：依赖不可用时返回 503。该判断必须排在凭据判断之后，
        // 避免用依赖故障掩盖「密码错误」这类确定性结果。
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::UNAVAILABLE, "player_store_unavailable",
                      "玩家档案暂时不可用，请稍后重试", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    // 第三步：创建会话。request_id 为空时不做幂等（视为一次性请求），
    // 非空时 SessionStore 保证同一 request_id 返回同一 Token。
    const std::string client_type = request->client_type().empty() ? "web" : request->client_type();
    std::string token;
    const StoreStatus created = sessions_->CreateSession(request_id, player.player_id(),
                                                         client_type, session_ttl_seconds_, &token);
    if (created != StoreStatus::kOk) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                      "session_store_unavailable", "会话存储暂时不可用，请稍后重试", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    response->set_token(token);
    response->set_expires_in_seconds(session_ttl_seconds_);
    *response->mutable_player() = player;
}

void GatewayServiceImpl::GetCurrentPlayer(::google::protobuf::RpcController* controller,
                                          const rgbt::gateway::v1::GetCurrentPlayerRequest* request,
                                          rgbt::gateway::v1::GetCurrentPlayerResponse* response,
                                          ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // 注意：Token 不能只从 request->token() 取。brpc 不把 HTTP 头映射进 protobuf
    // 字段，携带 Authorization 头的请求在这里会是空字符串。
    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    // 注意：错误体先写进局部变量，成功时不拷贝回 response。直接传
    // response->mutable_error() 会**提前创建** error 子消息，导致成功响应里也带一个
    // 空 error 字段，客户端据此会误判为失败。
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }

    const auto player = players_->FindByPlayerId(player_id);
    if (!player.has_value()) {
        const std::int32_t http_status =
            FillError(response->mutable_error(), ErrorCode::NOT_FOUND, "player_not_found",
                      "会话对应的玩家不存在", request_id);
        response->set_status_code(http_status);
        ApplyHttpStatus(controller, http_status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    *response->mutable_player() = player.value();
}

void GatewayServiceImpl::Logout(::google::protobuf::RpcController* controller,
                                const rgbt::gateway::v1::LogoutRequest* request,
                                rgbt::gateway::v1::LogoutResponse* response,
                                ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // 同 GetCurrentPlayer：Token 需要从 HTTP 头或 query string 中提取。
    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    const std::string token_error = ValidateToken(token);
    if (!token_error.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, token_error,
                      "Token 缺失或格式不合法", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    const StoreStatus status = sessions_->DeleteSession(token);
    if (status == StoreStatus::kUnavailable) {
        const std::int32_t http_status =
            FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                      "session_store_unavailable", "会话存储暂时不可用，请稍后重试", request_id);
        response->set_status_code(http_status);
        ApplyHttpStatus(controller, http_status);
        return;
    }

    // 会话本就不存在时也返回成功：登出是幂等操作，重复登出不应报错。
    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
}

std::int32_t GatewayServiceImpl::ResolvePlayerId(const std::string& token,
                                                 const std::string& request_id,
                                                 std::string* out_player_id,
                                                 rgbt::gateway::v1::Error* error) {
    const std::string token_error = ValidateToken(token);
    if (!token_error.empty()) {
        return FillError(error, ErrorCode::INVALID_ARGUMENT, token_error, "Token 缺失或格式不合法",
                         request_id);
    }

    SessionRecord session;
    const StoreStatus status = sessions_->GetSession(token, &session);
    if (status == StoreStatus::kUnavailable) {
        return FillError(error, ErrorCode::UNAVAILABLE, "session_store_unavailable",
                         "会话存储暂时不可用，请稍后重试", request_id);
    }
    if (status == StoreStatus::kNotFound) {
        // Token 格式合法但会话不存在：可能是过期或伪造，按未认证处理。
        return FillError(error, ErrorCode::UNAUTHENTICATED, "session_not_found",
                         "会话不存在或已过期", request_id);
    }

    *out_player_id = session.player_id;
    return 200;
}

void GatewayServiceImpl::FillMatchStatus(const MatchSnapshot& snapshot,
                                         rgbt::gateway::v1::MatchStatusInfo* out) {
    if (out == nullptr) {
        return;
    }
    out->set_state(MatchStateName(snapshot.state));
    out->set_match_id(snapshot.match_id);
    out->set_room_id(snapshot.room_id);
    for (const std::string& player_id : snapshot.player_ids) {
        out->add_player_ids(player_id);
    }
    out->set_queued_at_ms(snapshot.queued_at_ms);
    out->set_queue_size(snapshot.queue_size);
}

std::int32_t GatewayServiceImpl::HandleMatchFailure(MatchCallStatus status,
                                                    const std::string& request_id,
                                                    rgbt::gateway::v1::Error* error) {
    switch (status) {
        case MatchCallStatus::kOk:
            return 200;
        case MatchCallStatus::kUnavailable:
            // 对端未启动、超时或连接失败。返回 503 而不是 500：这是依赖不可用，
            // 不是本服务代码错误，恢复后无需重启 Gateway 即可继续匹配。
            return FillError(error, ErrorCode::UNAVAILABLE, "match_unavailable",
                             "匹配服务暂时不可用，请稍后重试", request_id);
        case MatchCallStatus::kInvalidArgument:
            return FillError(error, ErrorCode::INVALID_ARGUMENT, "match_invalid_argument",
                             "匹配请求参数不合法", request_id);
        case MatchCallStatus::kQueueFull:
            return FillError(error, ErrorCode::RESOURCE_EXHAUSTED, "match_queue_full",
                             "匹配队列已满，请稍后重试", request_id);
        case MatchCallStatus::kInternal:
            return FillError(error, ErrorCode::INTERNAL, "match_internal", "匹配服务返回未分类错误",
                             request_id);
    }
    return FillError(error, ErrorCode::INTERNAL, "match_internal", "匹配服务返回未知结果",
                     request_id);
}

namespace {

/// 房间状态的稳定字符串表示。与 api/proto/gateway.proto 的 RoomStateInfo.state 一致。
const char* RoomStateName(RoomState state) {
    switch (state) {
        case RoomState::kWaiting:
            return "waiting";
        case RoomState::kPlaying:
            return "playing";
        case RoomState::kFinishing:
            return "finishing";
        case RoomState::kFinished:
            return "finished";
        case RoomState::kAborted:
            return "aborted";
        case RoomState::kCreated:
        default:
            return "created";
    }
}

}  // namespace

void GatewayServiceImpl::FillRoomState(const RoomSnapshot& snapshot,
                                       rgbt::gateway::v1::RoomStateInfo* out) {
    if (out == nullptr) {
        return;
    }
    out->set_room_id(snapshot.room_id);
    out->set_match_id(snapshot.match_id);
    out->set_state(RoomStateName(snapshot.state));
    out->set_frame(snapshot.frame);
    out->set_finish_reason(snapshot.finish_reason);
    out->set_winner_id(snapshot.winner_id);
    out->set_started_at_ms(snapshot.started_at_ms);
    out->set_finished_at_ms(snapshot.finished_at_ms);

    for (const RoomPlayerSnapshot& player : snapshot.players) {
        rgbt::gateway::v1::RoomPlayerInfo* item = out->add_players();
        item->set_player_id(player.player_id);
        item->set_hp(player.hp);
        item->set_connected(player.connected);
    }
}

std::int32_t GatewayServiceImpl::HandleRoomFailure(RoomCallStatus status,
                                                   const std::string& request_id,
                                                   rgbt::gateway::v1::Error* error,
                                                   const char* not_found_reason) {
    switch (status) {
        case RoomCallStatus::kOk:
            return 200;
        case RoomCallStatus::kUnavailable:
            // 对端未启动、超时或连接失败。503 而不是 500：这是依赖不可用，
            // 恢复后无需重启 Gateway 即可继续。
            return FillError(error, ErrorCode::UNAVAILABLE, "room_unavailable",
                             "房间服务暂时不可用，请稍后重试", request_id);
        case RoomCallStatus::kStoreUnavailable:
            // Room 在，但它的存储（MySQL）不可用。与 room_unavailable 分开：
            // 排障时「哪个依赖挂了」是第一个要回答的问题。
            return FillError(error, ErrorCode::UNAVAILABLE, "result_store_unavailable",
                             "对局结果存储暂时不可用，请稍后重试", request_id);
        case RoomCallStatus::kResultPending:
            // 已分出胜负但结果尚未落库。503 而不是 404：结果**存在**，只是还查不到，
            // 调用方稍后重试即可。返回 404 会让客户端以为这局没有结果。
            return FillError(error, ErrorCode::UNAVAILABLE, "result_pending",
                             "对局已结束，结果仍在写入，请稍后重试", request_id);
        case RoomCallStatus::kNotFound:
            return FillError(error, ErrorCode::NOT_FOUND, not_found_reason, "房间或对局结果不存在",
                             request_id);
        case RoomCallStatus::kAlreadyFinished:
            return FillError(error, ErrorCode::CONFLICT, "room_already_finished",
                             "对局已结束，不再接受该操作", request_id);
        case RoomCallStatus::kInvalidArgument:
            return FillError(error, ErrorCode::INVALID_ARGUMENT, "room_invalid_argument",
                             "房间请求参数不合法", request_id);
        case RoomCallStatus::kInternal:
            return FillError(error, ErrorCode::INTERNAL, "room_internal", "房间服务返回未分类错误",
                             request_id);
    }
    return FillError(error, ErrorCode::INTERNAL, "room_internal", "房间服务返回未知结果",
                     request_id);
}

void GatewayServiceImpl::EnqueueMatch(::google::protobuf::RpcController* controller,
                                      const rgbt::gateway::v1::EnqueueMatchRequest* request,
                                      rgbt::gateway::v1::EnqueueMatchResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }

    // player_id 来自会话，请求体里就算带了这个字段也不会被读取。
    MatchSnapshot snapshot;
    const MatchCallStatus call = match_->Enqueue(player_id, request_id, &snapshot);
    const std::int32_t status = HandleMatchFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    FillMatchStatus(snapshot, response->mutable_match());
}

void GatewayServiceImpl::GetMatchStatus(::google::protobuf::RpcController* controller,
                                        const rgbt::gateway::v1::GetMatchStatusRequest* request,
                                        rgbt::gateway::v1::GetMatchStatusResponse* response,
                                        ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }

    MatchSnapshot snapshot;
    const MatchCallStatus call = match_->GetStatus(player_id, &snapshot);
    const std::int32_t status = HandleMatchFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    FillMatchStatus(snapshot, response->mutable_match());
}

void GatewayServiceImpl::CancelMatch(::google::protobuf::RpcController* controller,
                                     const rgbt::gateway::v1::CancelMatchRequest* request,
                                     rgbt::gateway::v1::CancelMatchResponse* response,
                                     ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }

    // 取消是幂等操作：玩家本来就不在队列中时，Match 同样返回成功状态。
    MatchSnapshot snapshot;
    const MatchCallStatus call = match_->Cancel(player_id, request_id, &snapshot);
    const std::int32_t status = HandleMatchFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    FillMatchStatus(snapshot, response->mutable_match());
}

void GatewayServiceImpl::JoinRoom(::google::protobuf::RpcController* controller,
                                  const rgbt::gateway::v1::JoinRoomRequest* request,
                                  rgbt::gateway::v1::JoinRoomResponse* response,
                                  ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string room_id = ExtractQueryParam(controller, "room_id", request->room_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }

    if (room_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "room_id_required",
                      "缺少 room_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    RoomSnapshot snapshot;
    const RoomCallStatus call = room_->Join(room_id, player_id, &snapshot);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    FillRoomState(snapshot, response->mutable_room());
}

void GatewayServiceImpl::SubmitInput(::google::protobuf::RpcController* controller,
                                     const rgbt::gateway::v1::SubmitInputRequest* request,
                                     rgbt::gateway::v1::SubmitInputResponse* response,
                                     ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string room_id = ExtractQueryParam(controller, "room_id", request->room_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }

    if (room_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "room_id_required",
                      "缺少 room_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    RoomSnapshot snapshot;
    // player_id 来自会话，请求体里即使带同名字段也会被忽略：
    // 否则任何登录用户都能替别人提交输入。
    const RoomCallStatus call = room_->SubmitAttack(room_id, player_id, &snapshot);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    FillRoomState(snapshot, response->mutable_room());
}

void GatewayServiceImpl::GetRoomState(::google::protobuf::RpcController* controller,
                                      const rgbt::gateway::v1::GetRoomStateRequest* request,
                                      rgbt::gateway::v1::GetRoomStateResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string room_id = ExtractQueryParam(controller, "room_id", request->room_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }
    // 这里只校验身份，不使用 player_id：房间快照对同局玩家是共享信息，
    // 而「谁能看这个房间」的判定在 Room 侧（非成员无法加入，因此也拿不到匹配结果）。
    (void)player_id;

    if (room_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "room_id_required",
                      "缺少 room_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    RoomSnapshot snapshot;
    const RoomCallStatus call = room_->GetState(room_id, &snapshot);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error);
    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    FillRoomState(snapshot, response->mutable_room());
}

void GatewayServiceImpl::GetMatchResult(::google::protobuf::RpcController* controller,
                                        const rgbt::gateway::v1::GetMatchResultRequest* request,
                                        rgbt::gateway::v1::GetMatchResultResponse* response,
                                        ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string token = ExtractToken(controller, request->token());
    const std::string request_id = request->request_id();
    const std::string match_id = ExtractQueryParam(controller, "match_id", request->match_id());

    std::string player_id;
    rgbt::gateway::v1::Error error;
    const std::int32_t auth = ResolvePlayerId(token, request_id, &player_id, &error);
    if (auth != 200) {
        *response->mutable_error() = error;
        response->set_status_code(auth);
        ApplyHttpStatus(controller, auth);
        return;
    }
    (void)player_id;

    if (match_id.empty()) {
        const std::int32_t status =
            FillError(response->mutable_error(), ErrorCode::INVALID_ARGUMENT, "match_id_required",
                      "缺少 match_id", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    if (room_ == nullptr) {
        const std::int32_t status = FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                                              "room_unavailable", "房间服务未配置", request_id);
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    MatchResultView view;
    const RoomCallStatus call = room_->GetResult(match_id, &view);
    const std::int32_t status = HandleRoomFailure(call, request_id, &error, "result_not_found");

    // 房间快照在成功与「结果待落库」两种情况下都可能存在，先填上。
    if (view.has_room) {
        FillRoomState(view.room, response->mutable_room());
    }

    if (status != 200) {
        *response->mutable_error() = error;
        response->set_status_code(status);
        ApplyHttpStatus(controller, status);
        return;
    }

    response->set_status_code(200);
    ApplyHttpStatus(controller, 200);
    if (view.has_result) {
        rgbt::gateway::v1::MatchResultInfo* result = response->mutable_result();
        result->set_match_id(view.result.match_id);
        result->set_room_id(view.result.room_id);
        result->set_winner_id(view.result.winner_id);
        result->set_player_count(view.result.player_count);
        result->set_started_at_ms(view.result.started_at_ms);
        result->set_finished_at_ms(view.result.finished_at_ms);
    }
}

bool GatewayServiceImpl::DependenciesHealthy() {
    // 只检查会话存储：账号目录是进程内数据，匹配服务在每次请求时单独判断
    // （它不可用只影响匹配接口，不应让整个 Gateway 显示为不健康）。
    return sessions_ != nullptr;
}

}  // namespace rgbt::gateway
