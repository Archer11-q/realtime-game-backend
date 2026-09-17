/// @file redis_session_store.hpp
/// @brief 基于 Redis 的会话存储实现。
///
/// 使用 hiredis 同步接口。理由：登录链路不在实时战斗热路径上，同步实现更简单、
/// 更容易推理；若后续压测显示该路径成为瓶颈，再评估异步化（需要 ADR 或压测证据）。

#ifndef RGBT_GATEWAY_REDIS_SESSION_STORE_HPP
#define RGBT_GATEWAY_REDIS_SESSION_STORE_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "session_store.hpp"

// 前向声明，避免把头文件依赖泄漏给调用方。定义在 redis_session_store.cpp。
struct redisContext;

namespace rgbt::gateway {

/// Redis 连接参数。
struct RedisOptions {
    std::string host = "127.0.0.1";
    std::int32_t port = 6379;

    /// 连接与命令超时（毫秒）。取正值，避免 Redis 不可用时请求长时间挂起。
    std::int32_t connect_timeout_ms = 500;
    std::int32_t command_timeout_ms = 500;
};

class RedisSessionStore : public SessionStore {
public:
    /// 构造并立即尝试建立连接。
    ///
    /// 连接失败不会让构造失败：这样服务仍可启动，并在每个请求上返回
    /// UNAVAILABLE，而不是启动即崩溃。Redis 恢复后无需重启服务。
    RedisSessionStore(std::string env_prefix, RedisOptions options,
                      std::int32_t session_ttl_seconds);

    ~RedisSessionStore() override;

    RedisSessionStore(const RedisSessionStore&) = delete;
    RedisSessionStore& operator=(const RedisSessionStore&) = delete;

    StoreStatus CreateSession(const std::string& request_id, const std::string& player_id,
                              const std::string& client_type, std::int32_t ttl_seconds,
                              std::string* out_token) override;

    StoreStatus GetSession(const std::string& token, SessionRecord* out_session) override;

    StoreStatus DeleteSession(const std::string& token) override;

    /// @brief 探测当前是否能与 Redis 通信。供健康检查使用。
    [[nodiscard]] bool IsHealthy();

    /// @brief 会话默认有效期（秒）。
    [[nodiscard]] std::int32_t session_ttl_seconds() const noexcept { return session_ttl_seconds_; }

private:
    /// 确保连接可用；必要时重连。返回 false 表示当前不可用。
    bool EnsureConnected();

    /// 执行命令并释放回复。
    void* ExecuteCommand(const char* format, ...);

    std::string KeyForSession(const std::string& token) const;
    std::string KeyForLoginIdempotency(const std::string& request_id) const;

    std::string env_prefix_;
    RedisOptions options_;
    std::int32_t session_ttl_seconds_;
    redisContext* context_ = nullptr;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_REDIS_SESSION_STORE_HPP
