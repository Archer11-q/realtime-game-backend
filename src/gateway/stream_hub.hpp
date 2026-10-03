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
///     * 只有**状态**变化才推送（不只是帧号：宽限期内帧号不变但"对方断线了"
///       必须推出去，见 TASK-016），避免空转刷屏。
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
/// 补发（TASK-017）为什么在 Tick 里而不是 Subscribe 里：Subscribe 运行在 brpc 的
/// 请求处理线程上，此刻响应还没提交，写出的数据要等 done 之后才以 chunked 发出，
/// 与后续 Tick 的写入**顺序无法保证**。放在 Tick 里之后顺序是确定的：
/// 补发（或 `stream.reset`）→ `session.ready` → 下一轮开始的实时推送。
///
/// 惰性清理：连接断开通过 `Write` 返回 false 发现，不依赖客户端发送任何东西。
/// 这是 SSE 的特点——客户端断开时服务端不会收到显式通知，只能靠写失败感知。

#ifndef RGBT_GATEWAY_STREAM_HUB_HPP
#define RGBT_GATEWAY_STREAM_HUB_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

/// 订阅参数（TASK-017）。
struct SubscribeOptions {
    /// SSE 的 `Last-Event-ID` 请求头解析出来的帧号。
    ///
    /// 三种取值对应三种行为：
    ///   * 有值 → 建立订阅时补发帧号大于它的快照（窗口内），窗口外 / id 超前
    ///     则发 `stream.reset`；
    ///   * 无值且 `last_event_id_malformed == false` → **第一次订阅**：不补发、
    ///     也不发 `stream.reset`，由第一次 Tick 正常推当前状态
    ///     （项目所有者 2026-10-02 裁决，理由见 TASK-017 实施记录）；
    ///   * `last_event_id_malformed == true` → 请求头存在但解析失败：发
    ///     `stream.reset`——这是客户端的 bug，不是第一次订阅，静默当成首次订阅
    ///     会让它以为自己拿到了连续的事件。
    ///
    /// 解析与合法性判断在 **Gateway 服务层**（`gateway_service.cpp`）完成，
    /// 因为只有那里能读到 HTTP 头；本模块不依赖 brpc，也就不该知道请求头长什么样。
    std::optional<std::int64_t> last_event_id;

    /// `Last-Event-ID` 请求头存在但无法解析成帧号。
    bool last_event_id_malformed = false;

    /// 建立这条订阅的 HTTP 请求的 request_id（TASK-018）。
    ///
    /// 存在订阅记录里，让"这条连接引发的所有 Room 调用"（上报 presence、查询
    /// 房间状态、补发历史）都带同一个关联键——否则订阅链路在跨服务日志里又会
    /// 断掉，而那正是本任务要消除的情况。
    std::string request_id;
};

/// 订阅的登记结果（TASK-017）。
///
/// 两个字段回答两个不同的问题：**"订阅登记上了吗"** 与 **"缺的帧补上了吗"**。
/// 后者必须如实上报，否则"补发失败"这件事在服务端没有任何痕迹——
/// 客户端只会看到帧号跳了一下，而它自己无法判断是不是漏帧。
struct SubscriptionReport {
    /// 订阅 id。0 表示订阅不存在。
    std::uint64_t id = 0;
    /// 订阅建立时发过 `stream.reset`（需要客户端按当前状态全量刷新）。
    bool reset_sent = false;
    /// 本次 reset 的原因字符串（稳定标识）。没有 reset 时为空。
    ///
    /// 取值：`id_malformed` / `id_out_of_window` / `id_ahead` / `id_current`。
    /// 由 StreamHub 与它的 stderr 日志共用同一份字符串，
    /// **不在这里再翻译一次**：出现一个新的原因而忘了更新日志，
    /// 就会造成"日志说一套、上报说另一套"。
    const char* reset_reason = "";
    /// 建立时按 `Last-Event-ID` 补发的帧数。
    std::size_t backfilled_frames = 0;
    /// 补发是否已经处理完。`false` 表示 Subscribe 之后还没有 Tick 跑过。
    bool backfill_done = false;
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
    ///
    /// **不在 Subscribe 里写任何数据**（包括补发）：Subscribe 运行在 brpc 的请求
    /// 处理线程上，此刻响应还未提交，写出去的内容要等 done 之后才会以 chunked
    /// 形式发出，与后续 Tick 的写入顺序无法保证。因此补发与 `session.ready`
    /// 都放在 Tick 里按固定顺序完成，见 Tick 的注释。
    ///
    /// @return 订阅登记结果。`id == 0` 表示 sink 为空、没有登记。
    [[nodiscard]] SubscriptionReport Subscribe(std::string player_id, std::string room_id,
                                               std::unique_ptr<EventSink> sink,
                                               SubscribeOptions options = {});

    /// @brief 取消订阅。sink 随之析构，流被关闭。
    /// 订阅不存在时静默返回（幂等）。
    void Unsubscribe(std::uint64_t id);

    /// @brief 查询某条订阅建立时的补发结果。
    ///
    /// 为什么需要它：补发发生在 Subscribe **之后**的某一次 Tick 里（理由见 Subscribe
    /// 的注释），因此 Subscribe 的返回值不可能包含补发结果。而"补发失败/窗口外"
    /// 必须在服务端留下痕迹——否则客户端只会看到帧号跳了一下，自己无从判断是否漏帧。
    ///
    /// @return 订阅不存在时 `id == 0`；`backfill_done == false` 表示还没来得及处理。
    [[nodiscard]] SubscriptionReport SubscribeReport(std::uint64_t id);

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

    /// @brief 累计补发出去的帧数（TASK-017）。
    ///
    /// 它是「`Last-Event-ID` 补发是否真的发生」这条链路的证据：补发本身在
    /// 端到端脚本里只能靠计时碰运气去触发，而这个计数可以由单元测试精确断言。
    [[nodiscard]] std::uint64_t BackfilledFrameCount();

    /// @brief 按原因统计累计发出的 `stream.reset` 次数（TASK-017）。
    ///
    /// 为什么按原因分开而不是只给一个总数：「窗口外」说明客户端落后得比缓冲还多，
    /// 「id 超前」说明客户端与服务端对不上，「首次订阅」则是正常路径。
    /// 压成一个数字之后，排障时看不出这次 reset 是不是问题。
    [[nodiscard]] std::uint64_t ResetEventCount(const std::string& reason);

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
        /// 见 SubscribeOptions::request_id：这条订阅的关联键。
        std::string request_id;
        std::shared_ptr<EventSink> sink;
        std::int64_t last_heartbeat_ms = 0;
        /// session.ready 是否已发送。放在 Tick 里发而不是 Subscribe 里发：
        /// Subscribe 运行在 brpc 的请求处理中，那时响应还没提交，
        /// 写出去的数据要等 done 之后才会以 chunked 形式发出（见 brpc 文档）。
        bool ready_sent = false;
        /// TASK-017：尚未处理的补发请求。由 Tick 在锁外执行，执行后清空——
        /// 补发只在订阅建立时做一次。
        ///
        /// `backfill_pending` 单独存在而不是靠 `backfill_from.has_value()` 判断：
        /// 「请求头缺失」与「请求头非法」这两种情况都可能在补发完成后仍然
        /// 没有帧号，用 optional 的空值表示"还没处理"会让它们混淆。
        bool backfill_pending = false;
        std::optional<std::int64_t> backfill_from;
        /// 请求头存在但解析失败。
        bool backfill_malformed = false;
        /// 补发结果。由 Tick 在阶段四写入，供 SubscribeReport 事后查询——
        /// 补发发生在 Subscribe 之后的某一次 Tick 里，因此 Subscribe 的返回值
        /// 无法直接包含它。
        bool backfill_done = false;
        bool reset_sent = false;
        const char* reset_reason = "";
        std::size_t backfilled_frames = 0;
    };

    /// 需要补发的订阅。锁内收集、锁外执行。
    struct BackfillTarget {
        std::uint64_t id = 0;
        std::shared_ptr<EventSink> sink;
        std::string room_id;
        /// 这条订阅的关联键（见 SubscribeOptions::request_id）。
        std::string request_id;
        /// 解析出来的 `Last-Event-ID`；为空表示请求头缺失。
        std::optional<std::int64_t> since_frame;
        /// 请求头存在但解析失败。与"缺失"分开：前者是客户端有问题，
        /// 后者只是第一次订阅，两者不该记成同一条日志。
        bool malformed = false;
    };

    /// 一次补发的结果。用结构体而不是哨兵返回值：这里有三个各自独立的结论
    /// （写了多少帧、连接是否还活着、有没有发 reset），压进一个整数会让调用方
    /// 必须记住编码规则，而记录规则的地方离使用的地方很远。
    struct BackfillOutcome {
        /// 是否真的处理过这次补发请求。
        ///
        /// `false` 只出现在"Room 不可用 / 取不到当前状态"这类**暂时性**情况下：
        /// 那时必须保留 `backfill_pending`，让下一轮 Tick 重试。否则一次抖动就会
        /// 静默丢掉这次补发请求，客户端只能靠帧号跳变自己发现。
        bool attempted = false;
        /// 写出去的帧数。
        std::size_t frames = 0;
        /// 连接已断，调用方应清理该订阅。
        bool write_failed = false;
        /// 发过 `stream.reset`。
        bool reset_sent = false;
        /// reset 的原因（稳定标识）；没有 reset 时为空字符串。
        const char* reset_reason = "";
    };

    /// 处理一个房间：轮询一次，变化时推给该房间的全部订阅者。
    void TickRoom(const std::string& room_id, const std::vector<SinkRef>& sinks,
                  const std::string& request_id, std::int64_t now_ms,
                  std::vector<std::uint64_t>* failed);

    /// @brief 处理一个订阅的补发（TASK-017）。**不得持锁调用**。
    ///
    /// 三种结果都必须显式区分：
    ///   * Room 不可用 → 不补发、不发事件、**不关闭连接**。沿用 TASK-009 的取舍：
    ///     一次抖动的代价不该是逼客户端重连。
    ///   * 窗口内 → 按帧号递增把缺失帧写出去，补发的事件同样带 `id:`。
    ///   * 窗口外 / id 超前 / id 无法解析 → 发 `stream.reset` 并附当前完整状态，
    ///     **明确告知"需要全量刷新"**，不假装补上了。
    [[nodiscard]] BackfillOutcome BackfillSubscription(const BackfillTarget& target);

    /// @brief 把一份快照登记为"本房间最近已推送的状态"。**自己加锁**。
    ///
    /// 补发与实时推送必须共用这一份登记：否则补发的最后一帧会被实时推送再写一遍。
    void SetLastPushedState(const std::string& room_id, const RoomSnapshot& snapshot,
                            bool terminal);

    /// 把"某个玩家的推送连接是否在线"上报给 Room（TASK-016）。**不得持锁调用**。
    /// 失败只记日志、不重试：这是尽力而为的事实同步，下一次连接变化会覆盖它。
    void ReportPresence(const std::string& room_id, const std::string& player_id, bool online,
                        const std::string& request_id);

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
    std::uint64_t backfilled_frame_count_ = 0;
    /// 原因 -> 累计发出的 stream.reset 次数。原因字符串都是字符串字面量，
    /// 生命周期与进程相同，因此这里直接以 `std::string` 为键，不做指针比较。
    std::unordered_map<std::string, std::uint64_t> reset_counts_;

    /// 每个房间最近一次已推送的状态签名（见 StateSignature）。
    ///
    /// 由 `mutex_` 保护：`TickRoom` 与 `SetLastPushedState` 都在 Tick 线程上，
    /// 但 `Subscribe` 会在 brpc 的请求线程上清掉本房间的条目（让新订阅者立刻有画面）。
    /// 这个表同时被两条路径读写，因此统一在 `mutex_` 下访问。
    std::unordered_map<std::string, std::string> last_pushed_state_;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_STREAM_HUB_HPP
