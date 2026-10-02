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

/// 上报玩家连接状态的结果（TASK-016）。
enum class PresenceOutcome {
    kOk,
    /// 该玩家不是这一局的成员。
    kNotAMember,
    /// 对局已结束，连接状态不再影响它。
    kAlreadyFinished,
    /// 玩家标识不合法。
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

    /// @brief 从快照记录重建房间（TASK-014，进程重启后的恢复路径）。
    ///
    /// 前提：调用方已经用 `ValidateRoomSnapshot` 确认过这条记录可用。
    /// 本函数只负责把状态装回去，不重复做合法性判断——两处都判会让
    /// "到底谁说了算"变得含糊，而校验逻辑已经是一个可单独测试的纯函数。
    ///
    /// **时间基准的处理是本函数最关键的部分**：
    ///   * PLAYING 房间的推进基准被重置为 `now_ms`，**不是** `snapshot_at_ms`。
    ///     这意味着停机期间本应推进的帧被**丢弃**，而不是在启动瞬间一次性补上。
    ///     理由与 `kMaxCatchUpFrames` 相同：补几百帧只会造成 CPU 尖峰，而那段
    ///     时间的输入本来就已经失去意义。代价是"对局在墙钟上被拉长"——
    ///     这是恢复边界的一部分，写在 docs/01-architecture.md 里。
    ///   * CREATED / WAITING 的等待超时基准用 `snapshot_at_ms` **近似**。
    ///     `rooms` 表没有 created_at_ms 这一列（TASK-013 的 schema 没有，
    ///     而 TASK-014 不新增列），因此恢复后的等待超时最多比未中断的房间
    ///     晚一个快照间隔。这是一个有上界的近似，不是不确定行为。
    ///   * FINISHING 房间把下一次落库重试设为**立即到期**：结果已经在内存里
    ///     消失过一次，没有必要再等一个完整的重试间隔。
    ///
    /// @return 记录不可用（例如玩家标识重复）时返回 nullopt。
    [[nodiscard]] static std::optional<BattleRoom> RestoreFrom(const RoomSnapshotRecord& record,
                                                               std::int64_t now_ms);

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

    /// @brief 上报某个玩家的推送连接状态（TASK-016）。**幂等**：重复上报同一状态无副作用。
    ///
    /// 为什么由 Gateway 上报而不是 Room 自己判断：SSE 是长连接，客户端断开时服务端
    /// 收不到显式通知，只能靠写失败感知（见 StreamHub）；那是 Gateway 才知道的事实。
    /// 而"这一局怎么办"是房间的权威状态，属于 Room。两者通过本方法的 RPC 对接。
    ///
    /// 行为：
    ///   * `online == false` → 记录断线时刻，进入宽限期（对局暂停推进）。
    ///   * `online == true` → 清除断线时刻，若无人断线则对局继续推进（血量与帧号不变，
    ///     所以"回来接着打"是精确的）。
    ///   * 对局已结束（FINISHING/FINISHED/ABORTED）→ 不再接受上报，返回 kAlreadyFinished。
    ///     理由：结束后的连接状态不会改变任何结果，让它"成功"会掩盖调用方的时序错误。
    PresenceOutcome SetPresence(const std::string& player_id, bool online, std::int64_t now_ms);

    /// @brief 是否有玩家处于宽限期。此时 Tick 不推进帧。
    [[nodiscard]] bool IsWaitingForReconnect() const noexcept;

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
    /// 只给 RestoreFrom 用的默认构造。
    ///
    /// 为什么放在 private：公开一个"什么都不填"的构造函数，就等于允许造出一个
    /// 没有房间号、没有玩家、状态还是 CREATED 的空壳房间。那种对象一旦流出，
    /// 出错的位置离原因就很远了。
    BattleRoom() = default;

    /// 找到一个玩家的可变状态。找不到返回 nullptr。
    [[nodiscard]] PlayerSnapshot* FindPlayer(const std::string& player_id);

    /// 找到玩家在 players_ 中的下标。找不到返回 nullopt。
    [[nodiscard]] std::optional<std::size_t> IndexOf(const std::string& player_id) const;

    /// 结束对局。调用方必须已确认对局处于 PLAYING。
    void Finish(FinishReason reason, std::string winner_id, std::int64_t now_ms);

    /// 作废对局（不产生胜负、不写结果）。调用方必须已确认对局尚未结束。
    void Abort(std::int64_t now_ms);

    /// @brief 宽限期结算：有人断线且已到期时结束或作废对局（TASK-016）。
    ///
    /// 规则（项目所有者确认）：
    ///   * 只有一方断线且已到期 → 断线方判负（FinishReason::kDisconnect）。
    ///   * 双方都断线且**最后一位**断线者也已到期 → 作废（不判定胜负，不写结果）。
    ///     为什么等最后一位而不是第一位：Gateway 重启会让所有订阅同时断开，
    ///     若按第一位到期就作废，任何一次 Gateway 重启都会立刻毁掉所有进行中的对局。
    ///
    /// @return true 表示对局已经因此结束/作废；false 表示仍在宽限期内（对局应暂停推进）。
    [[nodiscard]] bool ResolveReconnectGrace(std::int64_t now_ms);

    /// 每个玩家的连接运行态。与 players_ 下标对齐。
    ///
    /// 为什么不把这些字段直接放进 PlayerSnapshot：快照是对外契约（proto 一一对应），
    /// 而"断线时刻"纯属内部调度状态，泄漏进快照只会让调用方以为它有意义。
    struct PlayerRuntime {
        bool online = true;
        /// 断线时刻（毫秒）；在线时为 0。
        std::int64_t offline_since_ms = 0;
    };

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

    /// 与 players_ 下标对齐的连接运行态（TASK-016）。
    std::vector<PlayerRuntime> runtime_;

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
