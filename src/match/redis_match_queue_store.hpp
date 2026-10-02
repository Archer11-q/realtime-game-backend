/// @file redis_match_queue_store.hpp
/// @brief 匹配队列快照的 Redis 实现（TASK-015）。
///
/// 数据布局：一个 LIST，`<env>:match:queue`，每个元素是一行编码后的条目
/// （格式见 match_queue_store.hpp）。用 LIST 而不是 Hash/Set 的原因只有一个：
/// **FIFO 顺序是队列语义的一部分**，LIST 的天然有序正好承载它。
///
/// 写入是**整份替换**且用 `MULTI/EXEC` 包住（`DEL` + 若干 `RPUSH`），
/// 因此读取端看到的要么是旧的完整队列、要么是新的完整队列，不会是半份。
/// 没有这条保证时，进程在改写中途被杀会留下一个"只剩后半截"的队列——
/// 那比丢队列更糟，因为它是**看起来正常**的错误状态。
///
/// **线程安全**：Match 的 brpc worker 线程都会触发快照写入，因此本类内部加锁，
/// 并用 hiredis 的管道接口一次往返写完（队列上限 1000，逐条往返会让一次快照
/// 变成上千次 RTT）。所有管道回复**必须全部取走**，否则连接会与回复错位——
/// 这正是 TASK-014 在 Gateway 上实测到的那类缺陷（见 redis_session_store.hpp）。

#ifndef RGBT_MATCH_REDIS_MATCH_QUEUE_STORE_HPP
#define RGBT_MATCH_REDIS_MATCH_QUEUE_STORE_HPP

#include <cstdint>
#include <mutex>
#include <string>

#include "match_queue_store.hpp"

// 前向声明，避免把 hiredis 头文件泄漏给使用方。
struct redisContext;

namespace rgbt::match {

/// Redis 连接参数。与 gateway 的 RedisOptions 分开定义（两个服务各自独立配置），
/// 字段含义保持一致。
struct MatchRedisOptions {
    std::string host = "127.0.0.1";
    std::int32_t port = 6379;
    std::int32_t connect_timeout_ms = 500;
    std::int32_t command_timeout_ms = 500;
};

class RedisMatchQueueStore : public MatchQueueStore {
public:
    /// 构造时不因连接失败而失败：Redis 抖动不应导致 Match 反复重启。
    /// 不可用会体现为 Save/Load 返回 false，由调用方降级为纯内存。
    RedisMatchQueueStore(std::string env_prefix, MatchRedisOptions options);

    ~RedisMatchQueueStore() override;

    RedisMatchQueueStore(const RedisMatchQueueStore&) = delete;
    RedisMatchQueueStore& operator=(const RedisMatchQueueStore&) = delete;

    [[nodiscard]] bool Save(const MatchQueueSnapshot& snapshot) override;
    [[nodiscard]] bool Load(std::vector<MatchQueueSnapshotRow>* out_rows) override;
    [[nodiscard]] bool IsHealthy() override;

    /// @brief 队列快照的 Key。仅供验收脚本与文档引用。
    [[nodiscard]] std::string queue_key() const;

private:
    /// 确保连接可用；必要时重连。返回 false 表示当前不可用。
    /// **调用方必须已持有 mutex_**。
    bool EnsureConnected();

    /// 丢弃当前连接（下次调用会重连）。**调用方必须已持有 mutex_**。
    void DiscardConnection();

    std::string env_prefix_;
    MatchRedisOptions options_;
    redisContext* context_ = nullptr;

    /// 串行化对这条连接的访问。理由与 redis_session_store.hpp 的 mutex_ 相同：
    /// hiredis 的 redisContext 是有状态的一条连接，并发使用会让回复张冠李戴。
    mutable std::mutex mutex_;
};

}  // namespace rgbt::match

#endif  // RGBT_MATCH_REDIS_MATCH_QUEUE_STORE_HPP
