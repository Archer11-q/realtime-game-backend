/// @file gateway_service.hpp
/// @brief Gateway 的登录切片服务实现。
///
/// 覆盖接口（依据 docs/05-api-and-data.md 第 2 节的路径）：
///   POST /api/v1/login          登录并创建会话
///   GET  /api/v1/players/me     查询当前玩家
///   POST /api/v1/logout         结束会话
///
/// 服务边界（docs/01-architecture.md）：
///   Gateway 负责 HTTP 接入、鉴权、路由和限流，不保存战斗状态。
///   本实现只做接入与鉴权，不涉及匹配与房间。

#ifndef RGBT_GATEWAY_GATEWAY_SERVICE_HPP
#define RGBT_GATEWAY_GATEWAY_SERVICE_HPP

#include <cstdint>
#include <string>

#include "gateway.pb.h"
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
    GatewayServiceImpl(SessionStore* sessions, const PlayerDirectory* players,
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

    /// @brief 当前是否所有依赖都可用。供健康检查使用。
    [[nodiscard]] bool DependenciesHealthy();

private:
    /// 写统一的错误体，并返回对应的 HTTP 状态码。
    static std::int32_t FillError(rgbt::gateway::v1::Error* error,
                                  rgbt::gateway::v1::ErrorCode code, const std::string& reason,
                                  const std::string& message, const std::string& request_id);

    SessionStore* sessions_;
    const PlayerDirectory* players_;
    std::int32_t session_ttl_seconds_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_GATEWAY_SERVICE_HPP
