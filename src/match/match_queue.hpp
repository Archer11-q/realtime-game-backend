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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

/// 匹配队列。
class MatchQueue {
public:
    /// @param allocator 房间分配器，生命周期由调用方保证，不能为空。
    /// @param match_id_factory 匹配 ID 生成器；默认使用随机 Token。
    ///        注入的目的只是让测试可得到确定结果，生产路径不需要自定义。
    explicit MatchQueue(RoomAllocator* allocator,
                        std::function<std::string()> match_id_factory = {},
                        std::int64_t match_timeout_ms = kDefaultMatchTimeoutMs,
                        std::int64_t result_ttl_ms = kDefaultResultTtlMs,
                        std::size_t max_queue_size = kDefaultMaxQueueSize);

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

    /// @brief 取消匹配。
    /// @return true 表示确实从队列中移除，或玩家正处于房间分配中（取消请求已登记，
    ///         分配完成后不会让该玩家进入这一局）；false 表示玩家本来就不在队列中。
    ///         调用方按幂等成功处理 false，与登出的处理方式保持一致。
    bool Cancel(const std::string& player_id, std::int64_t now_ms);

    /// @brief 查询匹配状态。玩家没有任何记录时返回 kIdle。
    [[nodiscard]] MatchStatusSnapshot GetStatus(const std::string& player_id, std::int64_t now_ms);

    /// @brief 当前队列长度。仅供测试与可观测性使用。
    [[nodiscard]] std::size_t QueueSize();

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
    void SweepLocked(std::int64_t now_ms);

    /// 阶段一：从队列取出可配对的组并标记为分配中。调用方必须已持有 mutex_。
    [[nodiscard]] std::vector<PendingGroup> TakePairGroupsLocked();

    /// 阶段三：提交一次分配结果。调用方必须已持有 mutex_。
    ///
    /// @param room_id 分配到的房间号；空字符串表示分配失败。
    void CommitGroupLocked(const PendingGroup& group, const std::string& room_id,
                           std::int64_t now_ms);

    /// 阶段一 + 阶段三的组合：取组 -> **锁外**分配 -> 锁内提交。
    /// 调用方**不得**持有 mutex_。
    void RunPairingRound(std::int64_t now_ms);

    /// 如果上一次分配失败且玩家已退回队列，顺带重试一次配对。
    /// 只在真的有待重试的配对时才发起远程调用。
    void RetryPairingIfNeeded(std::int64_t now_ms);

    /// 从 FIFO 队列中移除指定玩家。调用方必须已持有 mutex_。
    void RemoveFromQueueLocked(const std::string& player_id);

    [[nodiscard]] MatchStatusSnapshot SnapshotLocked(const std::string& player_id) const;

    RoomAllocator* allocator_;
    std::function<std::string()> match_id_factory_;
    std::int64_t match_timeout_ms_;
    std::int64_t result_ttl_ms_;
    std::size_t max_queue_size_;

    mutable std::mutex mutex_;
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
