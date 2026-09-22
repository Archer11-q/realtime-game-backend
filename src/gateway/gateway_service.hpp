/// @file gateway_service.hpp
/// @brief Gateway 的对外服务实现。
///
/// 覆盖接口（依据 docs/05-api-and-data.md 第 2 节的路径）：
///   POST /api/v1/login                  登录并创建会话
///   GET  /api/v1/players/me             查询当前玩家
///   POST /api/v1/logout                 结束会话
///   POST /api/v1/matches                进入匹配（TASK-007）
///   GET  /api/v1/matches/current        查询匹配状态（TASK-007）
///   POST /api/v1/matches/current/cancel 取消匹配（TASK-007）
///
/// 服务边界（docs/01-architecture.md）：
///   Gateway 负责 HTTP 接入、鉴权、路由和限流，不保存战斗状态，也**不保存匹配队列**。
///   匹配接口只做「鉴权 -> 转给 Match -> 整理结果」，队列状态一律来自 Match Service。
///
/// 安全约定：匹配接口的 player_id **只能来自会话**，绝不使用请求体中的字段，
/// 否则任何登录用户都能替别人入队。

#ifndef RGBT_GATEWAY_GATEWAY_SERVICE_HPP
#define RGBT_GATEWAY_GATEWAY_SERVICE_HPP

#include <cstdint>
#include <string>

#include "gateway.pb.h"
#include "match_client.hpp"
#include "player_directory.hpp"
#include "session_store.hpp"

namespace rgbt::gateway {

/// 会话有效期（秒）。默认 7 天，同时是配置项便于测试与后续调整。
inline constexpr std::int32_t kDefaultSessionTtlSeconds = 7 * 24 * 60 * 60;

/// 账号与密码的长度上限。超长输入直接判为输入错误，避免把任意长度字符串
/// 传到底层依赖。
inline constexpr std::size_t kMaxAccountLength = 64;
inline constexpr std::size_t kMaxPasswordLength = 128;
inline constexpr std::size_t kMaxRequestIdLength = 64;
inline constexpr std::size_t kMaxTokenLength = 256;

class GatewayServiceImpl : public rgbt::gateway::v1::GatewayService {
public:
    GatewayServiceImpl(SessionStore* sessions, PlayerDirectory* players, MatchClient* match,
                       std::int32_t session_ttl_seconds = kDefaultSessionTtlSeconds);

    void Login(::google::protobuf::RpcController* controller,
               const rgbt::gateway::v1::LoginRequest* request,
               rgbt::gateway::v1::LoginResponse* response,
               ::google::protobuf::Closure* done) override;

    void GetCurrentPlayer(::google::protobuf::RpcController* controller,
                          const rgbt::gateway::v1::GetCurrentPlayerRequest* request,
                          rgbt::gateway::v1::GetCurrentPlayerResponse* response,
                          ::google::protobuf::Closure* done) override;

    void Logout(::google::protobuf::RpcController* controller,
                const rgbt::gateway::v1::LogoutRequest* request,
                rgbt::gateway::v1::LogoutResponse* response,
                ::google::protobuf::Closure* done) override;

    void EnqueueMatch(::google::protobuf::RpcController* controller,
                      const rgbt::gateway::v1::EnqueueMatchRequest* request,
                      rgbt::gateway::v1::EnqueueMatchResponse* response,
                      ::google::protobuf::Closure* done) override;

    void GetMatchStatus(::google::protobuf::RpcController* controller,
                        const rgbt::gateway::v1::GetMatchStatusRequest* request,
                        rgbt::gateway::v1::GetMatchStatusResponse* response,
                        ::google::protobuf::Closure* done) override;

    void CancelMatch(::google::protobuf::RpcController* controller,
                     const rgbt::gateway::v1::CancelMatchRequest* request,
                     rgbt::gateway::v1::CancelMatchResponse* response,
                     ::google::protobuf::Closure* done) override;

    /// @brief 当前是否所有依赖都可用。供健康检查使用。
    [[nodiscard]] bool DependenciesHealthy();

private:
    /// 写统一的错误体，并返回对应的 HTTP 状态码。
    static std::int32_t FillError(rgbt::gateway::v1::Error* error,
                                  rgbt::gateway::v1::ErrorCode code, const std::string& reason,
                                  const std::string& message, const std::string& request_id);

    /// @brief 由 Token 解析出会话所属的 player_id。
    /// @return 200 表示成功；其他值为已写入 error 的 HTTP 状态码，调用方直接返回即可。
    ///
    /// 抽成独立函数的原因：三个匹配接口都需要同一段「校验 Token 格式 -> 查会话 ->
    /// 取 player_id」逻辑，而这段逻辑里的错误码区分（400 / 401 / 503）是最容易写错
    /// 也最需要保持一致的地方。
    std::int32_t ResolvePlayerId(const std::string& token, const std::string& request_id,
                                 std::string* out_player_id, rgbt::gateway::v1::Error* error);

    /// 把 Match 返回的快照写入响应体。
    static void FillMatchStatus(const MatchSnapshot& snapshot,
                                rgbt::gateway::v1::MatchStatusInfo* out);

    /// @brief 把 Match 的调用结果转成错误体 + HTTP 状态码。
    /// @return 200 表示调用成功，调用方应继续填充 match 字段。
    std::int32_t HandleMatchFailure(MatchCallStatus status, const std::string& request_id,
                                    rgbt::gateway::v1::Error* error);

    SessionStore* sessions_;
    // 不加 const：接口方法本身不是 const（实现需要查询外部依赖），
    // 与 sessions_ 的写法保持一致。
    PlayerDirectory* players_;
    MatchClient* match_;
    std::int32_t session_ttl_seconds_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_GATEWAY_SERVICE_HPP
