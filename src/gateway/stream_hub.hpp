/// @file stream_hub.hpp
/// @brief SSE 订阅表与推送驱动。
///
/// 职责：维护「哪些连接订阅了哪个房间」，并按固定间隔把房间权威状态的变化推给
/// 订阅者。这是 TASK-009 的核心：它把「N 个客户端各自轮询 Gateway」变成
/// 「Gateway 扇入成一次内部轮询，再推给 N 个客户端」。
///
/// 事件从哪来（TASK-009 的关键设计决定）：
///   Room/Battle 是纯 brpc server，**不会主动推送**。因此由 Gateway 侧承担：
///     * 只对**有订阅者的房间**发起轮询（无订阅者完全不轮询，有单元测试锁定）；
///     * 同一个房间被多个客户端订阅时只轮询一次（扇入）；
///     * 只有帧号变化才推送，避免空转刷屏。
///   备选是让 Room 支持 brpc streaming RPC 订阅，那会引入 brpc 的流式语义，
///   Phase 1 不需要（见 docs/adr/0004-sse-instead-of-websocket.md 的取舍）。
///
/// 并发模型：
///   * `subscriptions_` 由 `mutex_` 保护，因为 Subscribe 来自 brpc 的请求线程、
///     Tick 来自推进线程。
///   * **持锁期间绝不调用 RoomClient 或 EventSink**。两者都可能阻塞（网络调用、
///     对端读取慢），持锁会把整张订阅表卡住。做法与 RoomManager 一致：先在锁内
///     取出本轮要处理的对象快照，再在锁外执行 I/O，最后回锁内提交结果。
///   * 出口用 `shared_ptr` 持有：即使订阅在写的过程中被移除，本次写仍然安全。
///
/// 惰性清理：连接断开通过 `Write` 返回 false 发现，不依赖客户端发送任何东西。
/// 这是 SSE 的特点——客户端断开时服务端不会收到显式通知，只能靠写失败感知。

#ifndef RGBT_GATEWAY_STREAM_HUB_HPP
#define RGBT_GATEWAY_STREAM_HUB_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "event_sink.hpp"
#include "room_client.hpp"

namespace rgbt::gateway {

/// 推送参数。
struct StreamHubOptions {
    /// 轮询房间状态的间隔。取 100 ms，与 Room 的帧长对齐：
    /// 更密没有意义（不会产生新帧），更疏则推送会比帧率还慢。
    std::int64_t poll_interval_ms = 100;

    /// 心跳间隔。/ 用来在房间长时间没有帧变化时保持连接不被中间层回收。
    std::int64_t heartbeat_interval_ms = 15 * 1000;
};

class StreamHub {
public:
    /// @param room 房间客户端。可为 nullptr（此时任何轮询都会失败），
    ///        用于只关心订阅表行为的单元测试。
    explicit StreamHub(RoomClient* room, StreamHubOptions options = {});
    ~StreamHub();

    StreamHub(const StreamHub&) = delete;
    StreamHub& operator=(const StreamHub&) = delete;
    StreamHub(StreamHub&&) = delete;
    StreamHub& operator=(StreamHub&&) = delete;

    /// @brief 注册一个订阅，接管 sink 的所有权。
    /// @return 订阅 id（大于 0）。同一个连接只应注册一次。
    std::uint64_t Subscribe(std::string player_id, std::string room_id,
                            std::unique_ptr<EventSink> sink);

    /// @brief 取消订阅。sink 随之析构，流被关闭。
    /// 订阅不存在时静默返回（幂等）。
    void Unsubscribe(std::uint64_t id);

    /// @brief 推进一轮：轮询房间、推送变化、发心跳、清理失效订阅。
    ///
    /// 由后台线程周期调用；单元测试直接调用它以获得确定的时间语义，
    /// 因此本方法不做 sleep，也不读系统时钟。
    void Tick(std::int64_t now_ms);

    /// @brief 启动后台推进线程。
    void Start();
    /// @brief 停止后台推进线程。可重复调用。
    void Stop();

    /// @brief 关闭全部订阅（连接被结束）。
    ///
    /// 优雅退出时必须调用：SSE 是长连接，不主动关闭的话它们会一直挂着，
    /// `brpc::Server::Stop()` 要等这些响应结束，进程就无法在预期时间内退出。
    void CloseAll();

    /// @brief 当前订阅数（连接数）。
    [[nodiscard]] std::size_t ConnectionCount();
    /// @brief 当前被订阅的不同房间数。
    [[nodiscard]] std::size_t SubscribedRoomCount();
    /// @brief 累计向 Room 发起的 GetRoomState 次数。
    ///
    /// 这个计数是「无订阅者不轮询」这条不变量的证据：端到端难以观测，
    /// 单元测试用它可以断言订阅前后计数的增量。
    [[nodiscard]] std::uint64_t RoomPollCount();

private:
    /// 订阅出口的引用。
    ///
    /// 用 shared_ptr 而不是裸指针：Tick 在锁外写数据，而订阅可能同时被移除
    /// （连接断开或客户端主动取消）。持有 shared_ptr 保证本次写期间对象存活，
    /// 避免了「用到已释放的出口」这类只在并发下才出现的崩溃。
    struct SinkRef {
        std::uint64_t id = 0;
        std::shared_ptr<EventSink> sink;
    };

    /// 订阅记录。
    struct Subscription {
        std::uint64_t id = 0;
        std::string player_id;
        std::string room_id;
        std::shared_ptr<EventSink> sink;
        std::int64_t last_heartbeat_ms = 0;
        /// session.ready 是否已发送。放在 Tick 里发而不是 Subscribe 里发：
        /// Subscribe 运行在 brpc 的请求处理中，那时响应还没提交，
        /// 写出去的数据要等 done 之后才会以 chunked 形式发出（见 brpc 文档）。
        bool ready_sent = false;
    };

    /// 处理一个房间：轮询一次，变化时推给该房间的全部订阅者。
    void TickRoom(const std::string& room_id, const std::vector<SinkRef>& sinks,
                  std::int64_t now_ms, std::vector<std::uint64_t>* failed);

    /// 把"某个玩家的推送连接是否在线"上报给 Room（TASK-016）。**不得持锁调用**。
    /// 失败只记日志、不重试：这是尽力而为的事实同步，下一次连接变化会覆盖它。
    void ReportPresence(const std::string& room_id, const std::string& player_id, bool online);

    /// 由房间状态生成"是否需要推送"的比较签名。
    ///
    /// 为什么不只比帧号（TASK-016）：宽限期内对局暂停推进、帧号不变，
    /// 而"对方断线了/回来了"正是那时最需要推给客户端的变化。
    [[nodiscard]] static std::string StateSignature(const RoomSnapshot& snapshot);

    RoomClient* room_;
    StreamHubOptions options_;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Subscription> subscriptions_;
    std::uint64_t next_id_ = 1;
    std::uint64_t room_poll_count_ = 0;

    /// 每个房间最近一次已推送的状态签名（见 StateSignature）。
    /// 只由 Tick 访问（单线程），因此不需要加锁。
    std::unordered_map<std::string, std::string> last_pushed_state_;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_STREAM_HUB_HPP
