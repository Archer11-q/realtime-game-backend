/// @file match_queue.hpp
/// @brief 匹配队列与最小配对逻辑（不依赖 brpc，可独立单元测试）。
///
/// 服务边界（docs/01-architecture.md 第 3 节）：
///   Match 维护匹配队列，处理进入、取消和超时，选择玩家并请求创建房间；
///   保证同一玩家不重复进入多个有效队列或匹配结果。
///   本文件只做队列与配对，不保存战斗状态、不直接向客户端发消息。
///
/// 四条关键设计决定：
///
/// 1. **队列放进程内存**（TASK-007 决策 1 选 A）。与架构文档把队列所有者记为 Match
///    一致，快照与重启恢复留 Phase 2。
///
/// 2. **单个互斥锁保护全部状态**。这直接给出「同一玩家不会同时出现在两个有效
///    队列或匹配结果中」这一架构要求：队列和结果表都以 player_id 为键，且所有
///    状态变更都在同一把锁内完成。
///    **已知限制**：这把锁只在单进程内有效。本项目**不做多实例部署**
///    （见 docs/adr/0003-scope-reduction.md），因此该限制不会在项目范围内触发；
///    不要为此引入分布式锁、etcd 或分片机制。
///
/// 3. **绝不在持锁期间调用远程**（TASK-008 起）。房间分配是一次 brpc 调用，
///    持锁调用意味着一次对端超时会把整个队列卡住。因此配对拆成两阶段：
///      阶段一（持锁）：从队列取出可配对的玩家，生成 match_id，标记为「分配中」
///      阶段二（放锁）：调用 RoomAllocator::Allocate
///      阶段三（持锁）：成功则提交为 matched，失败则把玩家放回队首
///    对外语义不变：分配期间玩家的状态仍报告为 kQueued，客户端只会在 queued 与
///    matched 之间切换，不会看到中间态。
///
/// 4. **时间由调用方注入**（`now_ms` 参数）。因此超时与结果过期可以用单元测试
///    精确验证，不需要 sleep，也不会出现「测试偶发失败」。
///
/// 5. **配对失败后惰性重试**。房间分配失败时玩家退回队列，但配对原本只在「有人入队」
///    时触发，于是这些人会一直等到下一个新玩家出现才可能被配上。Room 重启后
///    brpc channel 需要惰性重连，第一次调用必然失败，这个窗口很容易被撞上。
///    因此 `retry_pairing_` 记录「有失败的分配合并待重试」，并由客户端会持续调用的
///    `GetStatus` 顺带重试一次。玩家不需要重新入队。
///
/// 惰性淘汰：本类不创建定时器或后台线程，超时、过期与重试都只在每次调用时顺带结算。

#ifndef RGBT_MATCH_MATCH_QUEUE_HPP
#define RGBT_MATCH_MATCH_QUEUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "match_queue_store.hpp"
#include "room_allocator.hpp"

namespace rgbt::match {

/// 一局的人数。Phase 1 固定两人（决策 2 选 A）。
inline constexpr std::size_t kPlayersPerMatch = 2;

/// 排队超时（毫秒）。超过后本轮请求被淘汰，客户端需要重新入队。
inline constexpr std::int64_t kDefaultMatchTimeoutMs = 30 * 1000;

/// 匹配结果的保留时长（毫秒）。
///
/// 客户端靠轮询领取结果，所以结果必须在配对后保留一段时间；否则「配对成功」与
/// 「客户端下一次轮询」之间的竞态会导致结果丢失。超时后结果被清理，玩家回到空闲。
inline constexpr std::int64_t kDefaultResultTtlMs = 120 * 1000;

/// 队列长度上限。超过后拒绝新请求，避免无上限增长。
inline constexpr std::size_t kDefaultMaxQueueSize = 1000;

/// 幂等键与玩家标识的长度上限。超长输入直接判为输入错误，不进入队列。
inline constexpr std::size_t kMaxRequestIdLength = 64;
inline constexpr std::size_t kMaxPlayerIdLength = 64;

/// 一次匹配状态的快照。与 proto 的 `MatchState` 一一对应，但不依赖生成代码，
/// 因此本类可以在没有 protobuf 的情况下被测试。
struct MatchStatusSnapshot {
    enum class State {
        kIdle,
        kQueued,
        kMatched,
        kTimeout,
    };

    State state = State::kIdle;
    std::string request_id;
    std::string match_id;
    std::string room_id;
    std::vector<std::string> player_ids;
    std::int64_t queued_at_ms = 0;
    std::size_t queue_size = 0;
};

/// 入队结果。
enum class EnqueueOutcome {
    /// 已进入队列（可能在同一调用内就已经配对成功，以 status 为准）。
    kQueued,
    /// 玩家已在队列中，或已有尚未领取的匹配结果。按幂等成功处理，返回当前状态。
    kAlreadyQueued,
    /// 队列已满，属限流错误。
    kQueueFull,
    /// 玩家标识或幂等键不合法。
    kInvalidArgument,
};

/// 启动恢复的结果统计（TASK-015）。
///
/// 为什么要有一个结构而不是只打日志：启动日志会被后续输出冲掉，而
/// 「这次到底恢复了几个排队、几个已配对、丢了几条」是排障与验收都必须能一眼
/// 看到的事实。验收脚本也靠它断言"确实发生了恢复"，而不是只看玩家还在不在队列。
struct MatchRestoreReport {
    /// 从存储读到的行数。
    std::size_t scanned = 0;
    /// 重建为「排队中」的玩家数。
    std::size_t queued = 0;
    /// 重建为「已配对」的玩家数。
    std::size_t matched = 0;
    /// 因损坏或重复被跳过的行数（**不静默丢弃**：每一条都会记日志）。
    std::size_t dropped = 0;
    /// **已经超时所以没有恢复**的排队玩家数——停机时间算在等待里。
    /// 他们不会被装进内存变成 `timeout` 状态，而是干脆不恢复（等同于"该重新入队"）。
    std::size_t timed_out = 0;
    /// **结果保留期已过所以没有恢复**的已配对玩家数。
    std::size_t expired_results = 0;
    /// 存储不可用导致整体失败。此时一个条目也没有恢复。
    bool load_failed = false;
};

/// 匹配队列。
class MatchQueue {
public:
    /// @param allocator 房间分配器，生命周期由调用方保证，不能为空。
    /// @param match_id_factory 匹配 ID 生成器；默认使用随机 Token。
    ///        注入的目的只是让测试可得到确定结果，生产路径不需要自定义。
    /// @param store 队列快照存储（TASK-015）。可以为 nullptr（此时不做任何
    ///        快照与恢复），用于不关心持久化的测试。
    ///        **它的失败策略与对局结果相反**：快照可以丢弃、不重试、不阻塞入队；
    ///        因此 Redis 不可用时匹配照常工作，只是失去"重启可恢复"。
    explicit MatchQueue(RoomAllocator* allocator,
                        std::function<std::string()> match_id_factory = {},
                        std::int64_t match_timeout_ms = kDefaultMatchTimeoutMs,
                        std::int64_t result_ttl_ms = kDefaultResultTtlMs,
                        std::size_t max_queue_size = kDefaultMaxQueueSize,
                        MatchQueueStore* store = nullptr);

    MatchQueue(const MatchQueue&) = delete;
    MatchQueue& operator=(const MatchQueue&) = delete;

    /// @brief 进入匹配队列。
    ///
    /// 重复入队的语义（必须与 docs/05-api-and-data.md 第 3 节「状态冲突」一致）：
    ///   * 玩家已在排队 -> kAlreadyQueued，返回排队中的状态。
    ///   * 玩家已有未领取的匹配结果 -> kAlreadyQueued，返回该结果。
    ///     **不覆盖结果**，否则玩家会丢掉已经配好的局。
    ///   * 玩家处于超时状态 -> 允许重新入队（超时状态会被覆盖）。
    ///
    /// 本方法内部会调用 RoomAllocator（网络调用），但**不会持有锁**。
    EnqueueOutcome Enqueue(const std::string& player_id, const std::string& request_id,
                           std::int64_t now_ms);

    /// @brief 从存储恢复队列（TASK-015）。**在开始接受请求之前调用一次。**
    ///
    /// 行为：
    ///   * 只恢复「排队中」与「已配对」两类条目。已超时的条目不必持久化：
    ///     重启后玩家本就是空闲状态，对外语义等价。
    ///   * 排队条目按 `queued_at_ms` **稳定排序**重建 FIFO。稳定是必需的：
    ///     同一毫秒入队的两个人必须保持原来的先后。
    ///   * **重新判定超时**：把停机期间流逝的时间算进去。超过排队超时的条目
    ///     在恢复时就被淘汰，而不是让玩家重启后还能再排很久。
    ///   * 损坏的行**不静默丢弃**：跳过、计数、并逐条记日志。
    ///   * 存储不可用时**一个条目都不恢复**并如实报告 `load_failed`
    ///     ——空手启动比"以为恢复了其实没有"安全。
    [[nodiscard]] MatchRestoreReport Restore(std::int64_t now_ms);

    /// @brief 取消匹配。
    /// @return true 表示确实从队列中移除，或玩家正处于房间分配中（取消请求已登记，
    ///         分配完成后不会让该玩家进入这一局）；false 表示玩家本来就不在队列中。
    ///         调用方按幂等成功处理 false，与登出的处理方式保持一致。
    bool Cancel(const std::string& player_id, std::int64_t now_ms);

    /// @brief 查询匹配状态。玩家没有任何记录时返回 kIdle。
    [[nodiscard]] MatchStatusSnapshot GetStatus(const std::string& player_id, std::int64_t now_ms);

    /// @brief 当前队列长度。仅供测试与可观测性使用。
    [[nodiscard]] std::size_t QueueSize();

    /// TASK-019：快照写入成功/失败累计。供指标与测试读取。
    ///
    /// 为什么要在这里计数而不是让指标层去数日志：日志会滚动、会被采样，
    /// 而"降级为纯内存"这个事实必须有一个不会被冲掉的数字支撑。
    [[nodiscard]] std::uint64_t SnapshotWriteCount() const;
    [[nodiscard]] std::uint64_t SnapshotFailureCount() const;

    /// @brief 当前处于房间分配中的人数。仅供测试与可观测性使用。
    [[nodiscard]] std::size_t AllocatingCount();

private:
    struct Entry {
        MatchStatusSnapshot::State state = MatchStatusSnapshot::State::kIdle;
        std::string request_id;
        std::string match_id;
        std::string room_id;
        std::vector<std::string> player_ids;
        std::int64_t queued_at_ms = 0;
        /// 状态最近一次变化的时间。排队中是入队时间；配对或超时后是结果生成时间，
        /// 用于结算结果保留时长。
        std::int64_t stamp_ms = 0;
        /// 正在等待房间分配。此时玩家已从 queue_ 移出，但对外仍报告 kQueued。
        bool allocating = false;
        /// 分配期间收到了取消请求。分配完成后据此决定是否放弃这一局。
        bool cancel_requested = false;
    };

    /// 一次待分配的房间请求。
    struct PendingGroup {
        std::string match_id;
        std::vector<std::string> player_ids;
    };

    /// 惰性结算：淘汰超时的排队项，清理过期结果。调用方必须已持有 mutex_。
    /// @return true 表示状态发生了变化（调用方据此决定是否需要写快照）。
    [[nodiscard]] bool SweepLocked(std::int64_t now_ms);

    /// 阶段一：从队列取出可配对的组并标记为分配中。调用方必须已持有 mutex_。
    [[nodiscard]] std::vector<PendingGroup> TakePairGroupsLocked();

    /// 阶段三：提交一次分配结果。调用方必须已持有 mutex_。
    ///
    /// @param room_id 分配到的房间号；空字符串表示分配失败。
    void CommitGroupLocked(const PendingGroup& group, const std::string& room_id,
                           std::int64_t now_ms);

    /// 阶段一 + 阶段三的组合：取组 -> **锁外**分配 -> 锁内提交。
    /// 调用方**不得**持有 mutex_。
    /// @return true 表示本轮确实处理过分组（调用方据此决定是否需要写快照）。
    [[nodiscard]] bool RunPairingRound(std::int64_t now_ms);

    /// 如果上一次分配失败且玩家已退回队列，顺带重试一次配对。
    /// 只在真的有待重试的配对时才发起远程调用。
    /// @return true 表示本轮确实处理过分组。
    [[nodiscard]] bool RetryPairingIfNeeded(std::int64_t now_ms);

    /// 从 FIFO 队列中移除指定玩家。调用方必须已持有 mutex_。
    void RemoveFromQueueLocked(const std::string& player_id);

    [[nodiscard]] MatchStatusSnapshot SnapshotLocked(const std::string& player_id) const;

    /// 由内存状态生成快照内容。调用方必须已持有 mutex_。
    ///
    /// 「正在分配房间」的条目会作为**排队中**写入：重启后无法知道 CreateRoom
    /// 是否已经成功，而 Room 侧以 match_id 幂等，退回队列重新分配会拿回同一个
    /// room_id，因此退回比丢弃安全（TASK-015 决策 4）。
    [[nodiscard]] MatchQueueSnapshot MakeSnapshotLocked() const;

    /// @brief 把当前内存状态写进快照存储。**不持有任何锁即可调用。**
    ///
    /// 为什么不直接在状态变更处写：本项目明确避免"持锁做 I/O"——一次 Redis
    /// 超时会把整个队列卡住。因此这里的做法是**锁内取副本、锁外写**。
    ///
    /// 但只做到这一点还不够：两个线程可能各自取到较早的快照 S1 与较晚的快照 S2，
    /// 却按 S2 → S1 的顺序写入，把队列**写回退**。因此用 `snapshot_order_mutex_`
    /// 把「取快照 + 写快照」整体串行化，锁序固定为
    /// `snapshot_order_mutex_ → mutex_`（反向不存在，因此不会死锁）。
    void PersistSnapshot();

    RoomAllocator* allocator_;
    MatchQueueStore* store_;

    // TASK-019：快照写入计数。用原子而非普通成员：`PersistSnapshot` 由多个
    // brpc 工作线程触发（入队/取消/领取结果都会触发持久化）。
    std::atomic<std::uint64_t> snapshot_write_count_{0};
    std::atomic<std::uint64_t> snapshot_failure_count_{0};

    std::function<std::string()> match_id_factory_;
    std::int64_t match_timeout_ms_;
    std::int64_t result_ttl_ms_;
    std::size_t max_queue_size_;

    mutable std::mutex mutex_;
    /// 串行化「取快照 + 写快照」这一对操作，防止旧快照后写把队列写回退。
    /// 锁序固定为 snapshot_order_mutex_ -> mutex_（见 PersistSnapshot 的说明）。
    mutable std::mutex snapshot_order_mutex_;
    /// FIFO 顺序的 player_id。只保存仍在排队中的玩家（分配中的不在其中）。
    std::deque<std::string> queue_;
    /// 玩家 -> 状态。**以 player_id 为键**，这是「同一玩家不会重复出现在两个有效
    /// 匹配结果中」的实现基础。
    std::unordered_map<std::string, Entry> entries_;
    /// 是否存在一次失败的分配合并待重试。见文件头的第 5 条设计决定。
    bool retry_pairing_ = false;
};

}  // namespace rgbt::match

#endif  // RGBT_MATCH_MATCH_QUEUE_HPP
