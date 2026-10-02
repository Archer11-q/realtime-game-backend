/// @file battle_room.hpp
/// @brief 单个房间的生命周期与权威状态推进。
///
/// 职责（docs/01-architecture.md 第 3 节）：
///   创建、加入、开始和结束房间；保存当前房间的权威状态；按固定 tick 推进对局。
///   本文件**不**做持久化，也不依赖 brpc——结果落库由 RoomManager 通过
///   MatchResultWriter 接口完成，因此本类可以脱离数据库被单元测试。
///
/// 对局规则（TASK-008 决策 A，常量见 room_types.hpp）：
///   * 两人一局。双方到齐后进入 PLAYING，开始按 10 Hz 推进。
///   * 玩家输入只有「攻击」。**每个玩家每帧最多结算一次攻击**：同帧内的重复提交
///     会被合并，因为伤害是按帧结算的，允许同帧多次会变成「谁点得快谁赢」。
///   * 一方 HP 归零则该方落败；达到最大帧数时 HP 高者胜，相同为平局。
///
/// 并发模型：
///   本类**没有内部锁**。它只被 RoomManager 在持有管理器互斥锁的情况下访问，
///   与 src/match/match_queue.cpp 的做法一致。这样房间状态的每次迁移都是原子的，
///   不需要在多处重复加锁，也不会出现「半个状态」被外界看到。

#ifndef RGBT_ROOM_BATTLE_ROOM_HPP
#define RGBT_ROOM_BATTLE_ROOM_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "room_types.hpp"

namespace rgbt::room {

/// 加入房间的结果。
enum class JoinOutcome {
    /// 加入成功。**重复加入同一房间也返回本值**（幂等）。
    kOk,
    /// 该玩家不是这一局的成员。房间可能还有空位，但也不是给这个人的。
    kNotAMember,
    /// 对局已结束（FINISHING/FINISHED/ABORTED），不再接受加入。
    kAlreadyFinished,
    /// 玩家标识不合法（空或超长）。
    kInvalidArgument,
};

/// 提交输入的结果。
enum class SubmitOutcome {
    /// 已接受，将在下一帧结算。
    kAccepted,
    /// 玩家不在这个房间。
    kNotInRoom,
    /// 对局尚未开始（CREATED/WAITING）。
    kNotPlaying,
    /// 对局已结束。
    kAlreadyFinished,
    /// 输入不合法。
    kInvalidArgument,
};

/// 单个房间。
class BattleRoom {
public:
    /// @param room_id 房间标识，由 RoomManager 生成。
    /// @param match_id 产生本房间的匹配标识，同时是对局结果的幂等业务键。
    /// @param player_ids 本局应到场的玩家。顺序即快照中的玩家顺序。
    /// @param now_ms 创建时刻，用于「等待玩家加入」的超时判定。由调用方注入，
    ///        理由与 Tick 相同：让超时路径可以被单元测试精确验证。
    BattleRoom(std::string room_id, std::string match_id, std::vector<std::string> player_ids,
               std::int64_t now_ms);

    BattleRoom(const BattleRoom&) = delete;
    BattleRoom& operator=(const BattleRoom&) = delete;
    /// 允许移动：本类是纯值类型，没有与地址绑定的状态，测试也需要一个
    /// 「构造并返回」的工厂函数。拷贝仍然禁止——房间状态被悄悄复制成两份
    /// 是比编译错误严重得多的问题。
    BattleRoom(BattleRoom&&) = default;
    BattleRoom& operator=(BattleRoom&&) = default;
    ~BattleRoom() = default;

    [[nodiscard]] const std::string& room_id() const noexcept { return room_id_; }
    [[nodiscard]] const std::string& match_id() const noexcept { return match_id_; }
    [[nodiscard]] RoomPhase phase() const noexcept { return phase_; }
    [[nodiscard]] std::int64_t frame() const noexcept { return frame_; }

    /// @brief 加入房间。
    ///
    /// 幂等语义：已在房间内的玩家重复调用返回 kOk，不改变状态，也不重复放入位置。
    JoinOutcome Join(const std::string& player_id, std::int64_t now_ms);

    /// @brief 提交输入。仅 PLAYING 阶段接受。
    SubmitOutcome SubmitInput(const std::string& player_id, InputKind kind);

    /// @brief 按当前时间推进对局。
    ///
    /// @return true 表示状态发生了变化（帧号前进或进入结束流程）。
    ///
    /// 时间由调用方注入而不是内部读时钟：这样「HP 归零」「达到最大帧数」
    /// 「等待加入超时」都能被单元测试精确验证，不需要 sleep，也不会偶发失败。
    bool Tick(std::int64_t now_ms);

    /// @brief 结果写入成功，房间进入 FINISHED。由 RoomManager 在持久化成功后调用。
    void MarkResultPersisted();

    /// @brief 结果写入失败。房间停留在 FINISHING，等待下一次重试。
    /// @param now_ms 记录本次尝试时间，用于计算下一次重试时刻。
    void MarkResultFailed(std::int64_t now_ms);

    /// @brief 当前快照。
    [[nodiscard]] RoomSnapshot Snapshot() const;

    /// @brief 已生成的对局结果。仅在 phase >= FINISHING 时有值。
    [[nodiscard]] std::optional<MatchResultRecord> Result() const;

    /// @brief 快照环形缓冲中的历史帧数。仅供 Phase 2 与测试使用。
    [[nodiscard]] std::size_t SnapshotHistorySize() const noexcept { return history_.size(); }

    /// @brief 是否已经可以回收。
    [[nodiscard]] bool IsExpired(std::int64_t now_ms) const;

    /// @brief 到达结果重试时刻。仅在 FINISHING 阶段有意义。
    [[nodiscard]] bool ShouldRetryPersist(std::int64_t now_ms) const;

private:
    /// 找到一个玩家的可变状态。找不到返回 nullptr。
    [[nodiscard]] PlayerSnapshot* FindPlayer(const std::string& player_id);

    /// 找到玩家在 players_ 中的下标。找不到返回 nullopt。
    [[nodiscard]] std::optional<std::size_t> IndexOf(const std::string& player_id) const;

    /// 结束对局。调用方必须已确认对局处于 PLAYING。
    void Finish(FinishReason reason, std::string winner_id, std::int64_t now_ms);

    /// 推进一帧：结算本帧输入并检查胜负。
    void AdvanceOneFrame(std::int64_t now_ms);

    /// 记录一份快照到环形缓冲。
    void PushSnapshot();

    std::string room_id_;
    std::string match_id_;

    RoomPhase phase_ = RoomPhase::kCreated;
    FinishReason finish_reason_ = FinishReason::kNone;

    std::int64_t frame_ = 0;
    std::vector<PlayerSnapshot> players_;

    /// 每个玩家本帧是否有待结算的攻击。下标与 players_ 对齐。
    std::vector<bool> pending_attack_;

    /// 房间创建时间，用于等待加入的超时判定。
    std::int64_t created_at_ms_ = 0;
    /// 开局时间。
    std::int64_t started_at_ms_ = 0;
    /// 结束时间。
    std::int64_t finished_at_ms_ = 0;
    /// 上一次推进到的时间。帧号前进以它为基准，避免累计误差。
    std::int64_t last_tick_ms_ = 0;

    /// 最近一次结果写入尝试的时间。
    std::int64_t last_persist_attempt_ms_ = 0;

    std::string winner_id_;
    std::int32_t player_count_ = 0;

    /// 快照环形缓冲，供 Phase 2 的断线重连取历史帧。
    std::deque<RoomSnapshot> history_;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_BATTLE_ROOM_HPP
