/// @file room_manager.hpp
/// @brief 房间注册表、对局推进与结果落库。
///
/// 职责：
///   * 按 room_id 管理房间生命周期，按 match_id 保证创建幂等。
///   * 驱动所有房间的 tick 推进（由进程入口的定时线程调用）。
///   * 把已结束房间的结果**同步幂等**写入 MySQL；失败则持续重试。
///   * 回收已结束并超过保留期的房间。
///
/// 并发模型（与 src/match/match_queue.cpp 一致）：
///   **单个互斥锁保护全部状态**。这直接给出「同一 match_id 不会产生两个房间」
///   这一要求：索引与房间表的所有变更都在同一把锁内完成。
///   **已知限制**：这把锁只在单进程内有效。本项目不做多实例部署
///   （见 docs/adr/0003-scope-reduction.md），因此该限制不会在项目范围内触发。
///
/// 锁与远程调用：**持锁期间绝不调用 MySQL**。写结果与查结果都在锁外完成，
/// 只在读取/更新房间状态时短暂持锁。否则一次 MySQL 超时会把所有房间的 tick 卡住。

#ifndef RGBT_ROOM_ROOM_MANAGER_HPP
#define RGBT_ROOM_ROOM_MANAGER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "battle_room.hpp"
#include "match_result_writer.hpp"
#include "room_snapshot_reader.hpp"
#include "room_snapshot_writer.hpp"
#include "room_types.hpp"

namespace rgbt::room {

/// 启动恢复的结果统计。
///
/// 为什么要有一个结构而不是只打日志：启动日志会被后续输出冲掉，而
/// 「这次到底恢复了几个房间、拒绝了几个」是排障时必须能一眼看到的事实。
/// 验收脚本也靠它断言"确实发生了恢复"，而不是只看房间还能不能查到。
struct RestoreReport {
    /// 存储里扫描到的**未结束**快照行数。
    std::size_t scanned = 0;
    /// 成功重建的房间数。
    std::size_t restored = 0;
    /// 因快照损坏被拒绝、并已标记为 ABORTED 的数量。
    std::size_t rejected = 0;
    /// 存储不可用导致整体失败。此时一个房间也没有恢复。
    bool load_failed = false;
};

/// 创建房间的结果。
enum class CreateOutcome {
    /// 创建成功。**同一 match_id 重复调用也返回本值**，并返回同一个 room_id。
    kOk,
    /// 输入不合法（match_id 为空/超长，玩家列表为空，或玩家标识不合法）。
    kInvalidArgument,
    /// 房间号生成失败。属于内部错误。
    kInternal,
};

/// 查询对局结果的结果。
enum class ResultOutcome {
    /// 结果已确认写入并成功读出。
    kOk,
    /// 没有这条对局结果（对局不存在、已终止，或尚未结束）。
    kNotFound,
    /// 对局已分出胜负，但结果尚未落库。**调用方不得据此推断胜负**。
    kPending,
    /// 存储暂时不可用，可有限重试。
    kUnavailable,
};

/// 查询历史快照时，"请求的区间能不能补齐"的判定结果（TASK-017）。
///
/// 它与 RoomCallStatus 是**两个维度**：后者说"这次调用成不成功"，本枚举说
/// "成功的前提下，缺的帧补得齐吗"。合并成一个枚举会让调用方在
/// "Room 挂了"与"Room 好好的但这段历史已经不在内存里了"之间失去区分——
/// 前者可以稍后重试，后者重试一万次也一样。
enum class SnapshotRangeOutcome {
    /// 区间被完整覆盖，`out_snapshots` 里就是全部缺失帧。
    kOk,
    /// 房间不存在或已回收。
    kNotFound,
    /// 请求的帧号早于环形缓冲中最早的一帧：中间有一段已经不可恢复。
    kIncomplete,
    /// 请求的帧号晚于房间当前帧号（正常流程下不会出现）。
    kAhead,
};

/// 历史快照查询的返回结果。
struct SnapshotRange {
    SnapshotRangeOutcome outcome = SnapshotRangeOutcome::kNotFound;
    /// 帧号严格递增且不重复的缺失帧。仅 kOk 时保证完整。
    std::vector<RoomSnapshot> snapshots;
    /// 环形缓冲当前的帧号区间。缓冲为空时为 0，用于向调用方解释"为什么补不齐"。
    std::int64_t oldest_frame = 0;
    std::int64_t latest_frame = 0;
};

/// 房间注册表。
class RoomManager {
public:
    /// @param writer 对局结果存储。可以为 nullptr（此时任何落库都会失败），
    ///        主要用于不关心持久化的测试。
    /// @param snapshot_writer 房间快照存储（TASK-013）。可以为 nullptr
    ///        （此时不写快照），用于不关心快照的测试。
    ///        **它的失败策略与 writer 相反**：快照写失败只记日志、不重试，
    ///        下一次周期覆盖；对局结果写失败必须持续重试。见 room_snapshot_writer.hpp。
    /// @param room_id_factory 房间号生成器；默认使用随机 Token。
    ///        注入的目的只是让测试可得到确定结果，生产路径不需要自定义。
    /// @param snapshot_reader 房间快照读取器（TASK-014）。可以为 nullptr
    ///        （此时 Restore 什么也不做），用于不关心恢复的测试。
    explicit RoomManager(MatchResultWriter* writer, RoomSnapshotWriter* snapshot_writer = nullptr,
                         RoomSnapshotReader* snapshot_reader = nullptr,
                         std::function<std::string()> room_id_factory = {});

    RoomManager(const RoomManager&) = delete;
    RoomManager& operator=(const RoomManager&) = delete;

    /// @brief 创建房间。以 match_id 为幂等键。
    ///
    /// 幂等的意义：Match 在调用超时后会重试，若不幂等就会为同一局造出两个房间，
    /// 玩家被分到其中一个，另一个成为永远无人加入的孤儿。
    [[nodiscard]] CreateOutcome Create(const std::string& match_id,
                                       const std::vector<std::string>& player_ids,
                                       std::int64_t now_ms, std::string* out_room_id,
                                       RoomSnapshot* out_snapshot);

    /// @brief 加入房间。
    /// @return 房间不存在时返回 nullopt，否则返回房间层的判定结果。
    [[nodiscard]] std::optional<JoinOutcome> Join(const std::string& room_id,
                                                  const std::string& player_id, std::int64_t now_ms,
                                                  RoomSnapshot* out_snapshot);

    /// @brief 提交输入。
    /// @return 房间不存在时返回 nullopt，否则返回房间层的判定结果。
    [[nodiscard]] std::optional<SubmitOutcome> SubmitInput(const std::string& room_id,
                                                           const std::string& player_id,
                                                           InputKind kind,
                                                           RoomSnapshot* out_snapshot);

    /// @brief 上报某个玩家的推送连接状态（TASK-016）。
    ///
    /// 与 Join/SubmitInput 同构：返回 nullopt 表示房间不存在；否则返回房间层的判定结果。
    /// 存在原因见 battle_room.hpp 的 SetPresence：只有 Gateway 知道连接是否还在。
    [[nodiscard]] std::optional<PresenceOutcome> SetPresence(const std::string& room_id,
                                                             const std::string& player_id,
                                                             bool online, std::int64_t now_ms,
                                                             RoomSnapshot* out_snapshot);

    /// @brief 查询房间状态。
    /// @return false 表示房间不存在。
    [[nodiscard]] bool GetState(const std::string& room_id, std::int64_t now_ms,
                                RoomSnapshot* out_snapshot);

    /// @brief 查询对局结果。
    ///
    /// 优先看内存中的房间：只有状态为 FINISHED 才认为结果可信，此时**以数据库为准**
    /// 读出，保证返回的是已确认落库的内容。房间不在内存里（例如进程重启后）时
    /// 直接查数据库，这样历史对局仍然可查。
    [[nodiscard]] ResultOutcome GetResult(const std::string& match_id, std::int64_t now_ms,
                                          MatchResultRecord* out_record,
                                          RoomSnapshot* out_snapshot);

    /// @brief 从存储恢复未结束的房间（TASK-014）。**在开始接受请求之前调用一次。**
    ///
    /// 行为：
    ///   * 只恢复未结束的房间；已结束的没有恢复价值。
    ///   * 快照损坏的房间**不静默丢弃**：记下原因，并把该行改写成 ABORTED，
    ///     这样它在表里有一个明确的终态，而不是永远停在"看起来还在打"。
    ///   * `FINISHING` 的房间恢复后**立即**纳入落库重试，不等一个完整间隔。
    ///     这是 TASK-008 留下的已知限制（已结束但未落库的对局重启即丢失）的正解。
    ///   * 存储不可用时不恢复任何房间，并如实报告 `load_failed`
    ///     ——空手启动比"以为恢复了其实没有"安全。
    ///
    /// @param now_ms 恢复时刻。由调用方注入，理由与 Tick 相同：
    ///        让"停机期间的帧被丢弃"这条边界能被单元测试精确验证。
    [[nodiscard]] RestoreReport Restore(std::int64_t now_ms);

    /// @brief 查询帧号大于 since_frame 的历史快照（TASK-017）。
    ///
    /// 返回的 `snapshots` 保证帧号严格递增且不重复（由 `BattleRoom::SnapshotsAfter`
    /// 保证），因此 Gateway 可以按序直接写出，不需要再去重或排序。
    ///
    /// 完整性判定：把请求区间 `(since_frame, current_frame]` 与环形缓冲区间
    /// `[oldest, latest]`（左闭右闭）对照——
    ///   * `since_frame >= current_frame` → `kAhead`（客户端比服务端还新，状态不一致）
    ///   * `since_frame + 1 < oldest`、或缓冲为空而请求区间非空 → `kIncomplete`
    ///   * 其余 → `kOk`
    /// **不完整时不假装完整**：调用方（Gateway）据此给客户端一个明确的
    /// `stream.reset`，而不是让它从这段残缺的历史往后接。
    [[nodiscard]] SnapshotRange GetSnapshotsSince(const std::string& room_id,
                                                  std::int64_t since_frame, std::int64_t now_ms);

    /// @brief 推进所有房间，并处理结果落库与房间回收。由定时线程调用。
    void Tick(std::int64_t now_ms);

    /// @brief 当前房间总数。仅供指标与验收脚本使用。
    [[nodiscard]] std::size_t RoomCount();

    /// TASK-019：各阶段的房间数。**一次持锁数完**：若给每个阶段一个 getter，
    /// 按阶段导出指标就要加六把锁，而且六次读数之间房间状态会变，
    /// 面板上出现的"总数对不上"就是这么来的。
    [[nodiscard]] RoomPhaseCounts PhaseCounts();

    /// TASK-019：所有房间当前帧号之和，单调不减。
    ///
    /// 用它而不是"每次推进加一"的独立计数器：帧号本身就是累计量，
    /// 二者语义等价，但求和不需要在 `BattleRoom::Tick` 的热路径上再插一次写入，
    /// 也就不会出现"计数器与真实帧号不同步"这类只能靠对账才发现的问题。
    [[nodiscard]] std::uint64_t TotalFramesAdvanced();

    /// @brief 当前处于 PLAYING 的房间数。仅供指标与验收脚本使用。
    [[nodiscard]] std::size_t PlayingCount();

    /// @brief 当前处于 FINISHING（结果待落库）的房间数。
    ///
    /// 这个数字持续大于 0 说明存储写入一直失败——那是需要立刻看到的信号，
    /// 而不是等到客户端查不到结果才发现。
    [[nodiscard]] std::size_t PendingResultCount();

    /// @brief 累计写入成功的房间快照数。仅供验收脚本与指标使用。
    [[nodiscard]] std::uint64_t SnapshotWriteCount();

    /// @brief 累计写入失败的房间快照数。
    ///
    /// **它大于 0 本身不是故障信号**——快照可以丢弃，下一次周期会覆盖。
    /// 但若它随快照总数一起增长，说明快照存储长期不可用，
    /// 此时"重启可恢复"这件事实际上已经不成立了，需要被看到。
    [[nodiscard]] std::uint64_t SnapshotFailureCount();

private:
    /// 生成房间号。
    [[nodiscard]] std::string MakeRoomId() const;

    /// 回收已过期房间。调用方必须已持有 mutex_。
    void ReapExpiredLocked(std::int64_t now_ms);

    /// 每个房间的快照调度状态。
    struct SnapshotState {
        /// 上一次写快照的时间。
        std::int64_t last_write_ms = 0;
        /// 上一次写快照时房间处于哪个阶段。用于识别"刚刚进入终态"。
        RoomPhase last_phase = RoomPhase::kCreated;
        /// 是否已经写过至少一次。
        ///
        /// 为什么第一个快照不等间隔、立刻写：`rooms` 表的用途之一是"事后能查到
        /// 这里曾经有一局"。等一秒再写会让一个刚创建就异常退出的房间完全消失。
        bool written = false;
    };

    /// 判断某个房间本轮是否需要写快照。调用方必须已持有 mutex_。
    [[nodiscard]] static bool ShouldSnapshot(const SnapshotState& state, RoomPhase phase,
                                             std::int64_t now_ms);

    /// 由房间生成快照记录。调用方必须已持有 mutex_。
    [[nodiscard]] static RoomSnapshotRecord MakeSnapshotRecord(const BattleRoom& room,
                                                               std::int64_t now_ms);

    /// 把本轮到期的快照写入存储。**不持锁**。
    void FlushSnapshots(const std::vector<RoomSnapshotRecord>& records);

    /// 把恢复时被拒绝的快照改写成 ABORTED 并落库。**不持锁**。
    ///
    /// 为什么要有这一步：如果只是记一条日志然后把行留在原地，表里那一行会永远
    /// 停在 `playing`，下次启动又会被扫出来、又被拒绝——形成"每次启动都报同一个
    /// 错"的噪音，而且事后核对时看不出这一局到底怎么了。
    /// 日志也在这里打，而不是在持锁的扫描循环里打：持锁做 I/O 是本项目明确避免的。
    void RejectSnapshots(const std::vector<std::pair<RoomSnapshotRecord, std::string>>& rejected,
                         std::int64_t now_ms);

    MatchResultWriter* writer_;
    RoomSnapshotWriter* snapshot_writer_;
    RoomSnapshotReader* snapshot_reader_;
    std::function<std::string()> room_id_factory_;

    mutable std::mutex mutex_;
    /// room_id -> 房间。
    std::unordered_map<std::string, std::unique_ptr<BattleRoom>> rooms_;
    /// match_id -> room_id。创建幂等的实现基础。
    std::unordered_map<std::string, std::string> match_index_;
    /// match_id -> 快照调度状态。房间被回收时一并清理。
    std::unordered_map<std::string, SnapshotState> snapshot_state_;
    std::uint64_t snapshot_write_count_ = 0;
    std::uint64_t snapshot_failure_count_ = 0;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_ROOM_MANAGER_HPP
