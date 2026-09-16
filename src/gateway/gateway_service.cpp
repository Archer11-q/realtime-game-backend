#include "gateway_service.hpp"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <brpc/http_status_code.h>

#include <cstdint>
#include <string>

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

}  // namespace

GatewayServiceImpl::GatewayServiceImpl(SessionStore* sessions, const PlayerDirectory* players,
                                       std::int32_t session_ttl_seconds)
    : sessions_(sessions),
      players_(players),
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

    const std::string token = request->token();
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

    SessionRecord session;
    const StoreStatus status = sessions_->GetSession(token, &session);
    if (status == StoreStatus::kUnavailable) {
        const std::int32_t http_status =
            FillError(response->mutable_error(), ErrorCode::UNAVAILABLE,
                      "session_store_unavailable", "会话存储暂时不可用，请稍后重试", request_id);
        response->set_status_code(http_status);
        ApplyHttpStatus(controller, http_status);
        return;
    }
    if (status == StoreStatus::kNotFound) {
        // Token 格式合法但会话不存在：可能是过期或伪造，按未认证处理。
        const std::int32_t http_status =
            FillError(response->mutable_error(), ErrorCode::UNAUTHENTICATED, "session_not_found",
                      "会话不存在或已过期", request_id);
        response->set_status_code(http_status);
        ApplyHttpStatus(controller, http_status);
        return;
    }

    const auto player = players_->FindByPlayerId(session.player_id);
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

    const std::string token = request->token();
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

bool GatewayServiceImpl::DependenciesHealthy() {
    // 只检查会话存储：账号目录是进程内数据，不产生外部依赖。
    return sessions_ != nullptr;
}

}  // namespace rgbt::gateway
