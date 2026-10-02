/// @file redis_session_store.hpp
/// @brief 基于 Redis 的会话存储实现。
///
/// 使用 hiredis 同步接口。理由：登录链路不在实时战斗热路径上，同步实现更简单、
/// 更容易推理；若后续压测显示该路径成为瓶颈，再评估异步化（需要 ADR 或压测证据）。
///
/// **线程安全**：本类的 public 方法可以被多个线程并发调用（Gateway 的所有 brpc
/// worker 线程共用**同一个** `RedisSessionStore`），内部用一把互斥锁串行化。
/// 这不是预防性设计——TASK-014 期间实测到并发登录会让 hiredis 的回复错配：
/// 一个线程的 `+OK`（来自 `SET ... NX`）被另一个线程的 `GET` 当成 Token 读走，
/// 表现为登录响应里出现 `"token":"OK"`，另有部分请求返回 503 与一次段错误。
/// 详见成员 `mutex_` 的说明。

#ifndef RGBT_GATEWAY_REDIS_SESSION_STORE_HPP
#define RGBT_GATEWAY_REDIS_SESSION_STORE_HPP

#include <cstdint>
#include <memory>
#include <mutex>
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
    /// **调用方必须已持有 mutex_**（它读写 context_）。
    bool EnsureConnected();

    /// 执行命令并释放回复。**调用方必须已持有 mutex_**。
    ///
    /// 注意：回复的配对正确性依赖"同一条命令发出到取回回复之间没有别的线程插队"，
    /// 因此加锁必须覆盖 public 方法的**整个**调用序列，而不是只覆盖这一条命令。
    void* ExecuteCommand(const char* format, ...);

    std::string KeyForSession(const std::string& token) const;
    std::string KeyForLoginIdempotency(const std::string& request_id) const;

    std::string env_prefix_;
    RedisOptions options_;
    std::int32_t session_ttl_seconds_;
    redisContext* context_ = nullptr;

    /// @brief 串行化对这条 Redis 连接的访问。**本类是线程安全的，靠的就是它。**
    ///
    /// 为什么必须有（TASK-014 实测，验收脚本 7e 直接抓到）：
    ///   hiredis 的 `redisContext` 是一条**有状态的连接**：它内部只有一个读缓冲和
    ///   一个错误字段，`redisvCommand` 是"写请求 + 读回复"两步。两个线程共用它时，
    ///   回复会张冠李戴——实测到的直接证据是并发登录的响应体里出现
    ///   `"token":"OK"`（`SET ... NX` 的 `+OK` 状态回复被另一个线程的 `GET` 读走）。
    ///   同一现象还会表现为 `session_store_unavailable` 的 503，以及一次段错误。
    ///
    ///   触发条件是"同时有多个请求"，而 Gateway 的所有 brpc worker 线程共用
    ///   同一个 store 对象，因此这不是理论问题：并发登录必然踩到。
    ///
    /// 已知代价：锁覆盖整个业务方法（含可能的重连），因此 Redis 慢时会串行化这
    ///   条连接上的调用。这与 MySQL 侧同理——一条连接本来就无法并发执行两条命令；
    ///   真正的扩容手段是连接池，不是去掉这把锁（当前没有容量证据，见 CLAUDE.md 第 6 条）。
    mutable std::mutex mutex_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_REDIS_SESSION_STORE_HPP
