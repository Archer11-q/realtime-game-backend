#include "battle_room.hpp"

#include <algorithm>
#include <utility>

namespace rgbt::room {

BattleRoom::BattleRoom(std::string room_id, std::string match_id,
                       std::vector<std::string> player_ids, std::int64_t now_ms)
    : room_id_(std::move(room_id)), match_id_(std::move(match_id)), created_at_ms_(now_ms) {
    players_.reserve(player_ids.size());
    for (const std::string& player_id : player_ids) {
        PlayerSnapshot snapshot;
        snapshot.player_id = player_id;
        snapshot.hp = kInitialHp;
        // 尚未加入房间。TASK-008 用 connected 表达「当前在房间内」，
        // 而不是「网络是否连通」——断线重连与宽限期属 Phase 2。
        snapshot.connected = false;
        players_.push_back(std::move(snapshot));
    }
    pending_attack_.assign(players_.size(), false);
    // TASK-016：新房间的玩家视为在线。真正的"是否连通"由 Gateway 通过
    // SetPlayerPresence 上报——房间创建时还没有任何推送连接，但那不等于"断线"。
    runtime_.assign(players_.size(), PlayerRuntime{});
    player_count_ = static_cast<std::int32_t>(players_.size());
    PushSnapshot();
}

std::optional<BattleRoom> BattleRoom::RestoreFrom(const RoomSnapshotRecord& record,
                                                  std::int64_t now_ms) {
    // 玩家数是硬前提：少一个人就凑不成一局，这里直接拒绝而不是补一个空位。
    if (record.players.size() != kPlayersPerRoom) {
        return std::nullopt;
    }
    // 标识必须非空且互不相同。这两条 ValidateRoomSnapshot 也会查，
    // 这里重复一次是**有意的第二道防线**：RestoreFrom 的契约是"调用方已验证"，
    // 但一个不校验的调用方不应该能把"两个位置是同一个人"的房间装进内存。
    for (std::size_t i = 0; i < record.players.size(); ++i) {
        if (record.players[i].player_id.empty()) {
            return std::nullopt;
        }
        for (std::size_t j = i + 1; j < record.players.size(); ++j) {
            if (record.players[i].player_id == record.players[j].player_id) {
                return std::nullopt;
            }
        }
    }

    BattleRoom room;
    room.room_id_ = record.room_id;
    room.match_id_ = record.match_id;
    room.phase_ = record.phase;
    room.finish_reason_ = record.finish_reason;
    room.frame_ = record.frame;
    room.winner_id_ = record.winner_id;
    room.started_at_ms_ = record.started_at_ms;
    room.finished_at_ms_ = record.finished_at_ms;
    room.player_count_ = static_cast<std::int32_t>(kPlayersPerRoom);

    room.players_.reserve(kPlayersPerRoom);
    for (const RoomPlayerRecord& player : record.players) {
        PlayerSnapshot snapshot;
        snapshot.player_id = player.player_id;
        snapshot.hp = player.hp;
        // 表里叫 joined、内存里叫 connected，指的是同一件事：
        // 「是否已在房间内」，不是网络是否连通（见 room_types.hpp）。
        snapshot.connected = player.joined;
        room.players_.push_back(std::move(snapshot));
    }
    room.pending_attack_.assign(kPlayersPerRoom, false);
    // TASK-016：恢复出来的房间把双方都当作在线，宽限计时清零。
    //
    // 这是**有意的近似**：presence 不落库（rooms 表没有这一列，本任务不加列），
    // 而 Room 重启时 Gateway 的订阅本来就全断了，客户端重连后会重新上报。
    // 代价是"Room 重启会重置宽限期"，写进 docs/01-architecture.md 的已知限制。
    room.runtime_.assign(kPlayersPerRoom, PlayerRuntime{});

    // 等待超时的基准用快照时刻近似。表里没有 created_at_ms，而本任务不新增列；
    // 代价是恢复后的等待超时最多晚一个快照间隔（有上界，见头文件说明）。
    room.created_at_ms_ = record.snapshot_at_ms > 0 ? record.snapshot_at_ms : now_ms;
    // 推进基准重置为重启时刻：停机期间的帧被丢弃，不做补偿。
    room.last_tick_ms_ = now_ms;
    // 立即到期：FINISHING 的结果已经在内存里消失过一次，再等一个完整间隔没有意义。
    room.last_persist_attempt_ms_ = now_ms - kResultRetryIntervalMs;

    // 把恢复后的状态推入快照环形缓冲：重连的客户端取历史帧时应当能拿到当前状态。
    room.PushSnapshot();
    return room;
}

PlayerSnapshot* BattleRoom::FindPlayer(const std::string& player_id) {
    for (PlayerSnapshot& player : players_) {
        if (player.player_id == player_id) {
            return &player;
        }
    }
    return nullptr;
}

std::optional<std::size_t> BattleRoom::IndexOf(const std::string& player_id) const {
    for (std::size_t i = 0; i < players_.size(); ++i) {
        if (players_[i].player_id == player_id) {
            return i;
        }
    }
    return std::nullopt;
}

JoinOutcome BattleRoom::Join(const std::string& player_id, std::int64_t now_ms) {
    if (player_id.empty() || player_id.size() > kMaxIdLength) {
        return JoinOutcome::kInvalidArgument;
    }
    if (phase_ == RoomPhase::kFinishing || phase_ == RoomPhase::kFinished ||
        phase_ == RoomPhase::kAborted) {
        return JoinOutcome::kAlreadyFinished;
    }

    PlayerSnapshot* player = FindPlayer(player_id);
    if (player == nullptr) {
        // 不在本局名单里。房间可能还有空位，但那不是给这个人的。
        return JoinOutcome::kNotAMember;
    }
    if (player->connected) {
        // 重复加入按幂等成功处理，不改变任何状态。
        // 与登出、取消匹配的处理方式保持一致（docs/05-api-and-data.md 第 3 节）。
        return JoinOutcome::kOk;
    }

    player->connected = true;
    if (phase_ == RoomPhase::kCreated) {
        phase_ = RoomPhase::kWaiting;
    }

    bool all_joined = true;
    for (const PlayerSnapshot& other : players_) {
        if (!other.connected) {
            all_joined = false;
            break;
        }
    }

    if (all_joined) {
        // 双方到齐 -> 开局。帧号从 0 开始，基准时间设为现在，
        // 因此第一帧会在 kFrameIntervalMs 之后推进。
        phase_ = RoomPhase::kPlaying;
        started_at_ms_ = now_ms;
        last_tick_ms_ = now_ms;
        frame_ = 0;
    }

    PushSnapshot();
    return JoinOutcome::kOk;
}

SubmitOutcome BattleRoom::SubmitInput(const std::string& player_id, InputKind kind) {
    if (player_id.empty() || player_id.size() > kMaxIdLength) {
        return SubmitOutcome::kInvalidArgument;
    }
    if (kind != InputKind::kAttack) {
        // 当前只有一种输入类型。走到这里说明调用方传了未定义值。
        return SubmitOutcome::kInvalidArgument;
    }
    if (phase_ == RoomPhase::kFinishing || phase_ == RoomPhase::kFinished ||
        phase_ == RoomPhase::kAborted) {
        return SubmitOutcome::kAlreadyFinished;
    }
    if (phase_ != RoomPhase::kPlaying) {
        return SubmitOutcome::kNotPlaying;
    }

    const std::optional<std::size_t> index = IndexOf(player_id);
    if (!index.has_value()) {
        return SubmitOutcome::kNotInRoom;
    }

    // 同帧内的重复提交被合并：伤害按帧结算，允许同帧多次会变成「谁点得快谁赢」，
    // 那是延迟决定胜负，不是对局规则。
    pending_attack_[*index] = true;
    return SubmitOutcome::kAccepted;
}

bool BattleRoom::Tick(std::int64_t now_ms) {
    if (phase_ == RoomPhase::kCreated || phase_ == RoomPhase::kWaiting) {
        // 匹配成功却始终没人加入：房间不能被永久占着，否则就是资源泄漏。
        if (now_ms - created_at_ms_ >= kWaitingTimeoutMs) {
            Abort(now_ms);
            return true;
        }
        return false;
    }

    if (phase_ != RoomPhase::kPlaying) {
        return false;
    }

    // TASK-016：有人断线时对局**暂停推进**（帧与血量都冻结），只结算宽限期。
    // 先结算再决定是否返回：到期必须能触发结束，而"仍在宽限期内"就是不推进。
    //
    // 注意暂停只影响推进，不影响 `last_tick_ms_` 的基准长度：这里直接返回，
    // 下一次恢复推进时 elapsed 会包含整个暂停时长，因此下面会按
    // kMaxCatchUpFrames 截断——断线这段时间不会被一次性补成几十帧，
    // 这正是"暂停"而不是"补帧"。
    if (IsWaitingForReconnect()) {
        if (ResolveReconnectGrace(now_ms)) {
            return true;
        }
        // 冻结推进基准，否则恢复推进时会把暂停期当成"该补的帧"。
        last_tick_ms_ = now_ms;
        return false;
    }

    if (now_ms < last_tick_ms_) {
        // 时钟回退。防御：不推进，也不倒退 last_tick_ms_。
        return false;
    }

    const std::int64_t elapsed_ms = now_ms - last_tick_ms_;
    std::int64_t frames = elapsed_ms / kFrameIntervalMs;
    if (frames <= 0) {
        return false;
    }

    if (frames > kMaxCatchUpFrames) {
        // 进程被挂起或调度延迟导致一次跳过多帧。超过上限的部分直接丢弃：
        // 把基准时间前移到「只差上限帧」的位置，避免下一轮继续补偿，
        // 也避免 CPU 尖峰。这段时间的输入本来也已经失去意义。
        const std::int64_t dropped_ms = (frames - kMaxCatchUpFrames) * kFrameIntervalMs;
        last_tick_ms_ += dropped_ms;
        frames = kMaxCatchUpFrames;
    }

    bool changed = false;
    for (std::int64_t i = 0; i < frames && phase_ == RoomPhase::kPlaying; ++i) {
        last_tick_ms_ += kFrameIntervalMs;
        AdvanceOneFrame(now_ms);
        changed = true;
    }
    return changed;
}

void BattleRoom::AdvanceOneFrame(std::int64_t now_ms) {
    ++frame_;

    // 结算本帧输入。两人房间中，每个玩家的对手就是另一个人。
    for (std::size_t i = 0; i < players_.size(); ++i) {
        if (!pending_attack_[i]) {
            continue;
        }
        pending_attack_[i] = false;
        if (players_.size() < 2) {
            continue;
        }
        const std::size_t target = (i + 1) % players_.size();
        players_[target].hp -= kAttackDamage;
        if (players_[target].hp < 0) {
            players_[target].hp = 0;
        }
    }

    // 判定一：是否有人 HP 归零。
    std::vector<std::size_t> alive;
    alive.reserve(players_.size());
    for (std::size_t i = 0; i < players_.size(); ++i) {
        if (players_[i].hp > 0) {
            alive.push_back(i);
        }
    }
    if (alive.size() <= 1) {
        // 恰好一人存活即该人获胜；无人存活（同帧互杀）按平局处理。
        std::string winner;
        if (alive.size() == 1) {
            winner = players_[alive[0]].player_id;
        }
        Finish(FinishReason::kHpZero, std::move(winner), now_ms);
        return;
    }

    // 判定二：达到最大帧数，按 HP 判定。相同则平局。
    if (frame_ >= kMaxFrames) {
        std::string winner;
        if (players_.size() == 2) {
            if (players_[0].hp > players_[1].hp) {
                winner = players_[0].player_id;
            } else if (players_[1].hp > players_[0].hp) {
                winner = players_[1].player_id;
            }
        }
        Finish(FinishReason::kTimeout, std::move(winner), now_ms);
        return;
    }

    PushSnapshot();
}

void BattleRoom::Finish(FinishReason reason, std::string winner_id, std::int64_t now_ms) {
    phase_ = RoomPhase::kFinishing;
    finish_reason_ = reason;
    winner_id_ = std::move(winner_id);
    finished_at_ms_ = now_ms;
    // 置 0 而不是 now_ms：让 RoomManager 在下一次 Tick 立刻尝试首次写入，
    // 而不是白等一个重试间隔。写入失败后由 MarkResultFailed 记录本次时间。
    last_persist_attempt_ms_ = 0;
    PushSnapshot();
}

bool BattleRoom::IsWaitingForReconnect() const noexcept {
    for (const PlayerRuntime& runtime : runtime_) {
        if (!runtime.online) {
            return true;
        }
    }
    return false;
}

PresenceOutcome BattleRoom::SetPresence(const std::string& player_id, bool online,
                                        std::int64_t now_ms) {
    if (player_id.empty() || player_id.size() > kMaxIdLength) {
        return PresenceOutcome::kInvalidArgument;
    }
    if (phase_ == RoomPhase::kFinishing || phase_ == RoomPhase::kFinished ||
        phase_ == RoomPhase::kAborted) {
        return PresenceOutcome::kAlreadyFinished;
    }

    const std::optional<std::size_t> index = IndexOf(player_id);
    if (!index.has_value()) {
        return PresenceOutcome::kNotAMember;
    }

    PlayerRuntime& runtime = runtime_[*index];
    if (online) {
        runtime.online = true;
        runtime.offline_since_ms = 0;
        // 回来后不做任何血量/帧号补偿：暂停期间两者都没有变化，
        // 因此"接着打"是精确的，这正是选择"暂停推进"而不是"继续推进"的收益。
        players_[*index].online = true;
    } else if (runtime.online) {
        runtime.online = false;
        runtime.offline_since_ms = now_ms;
        players_[*index].online = false;
        PushSnapshot();  // 让订阅者立刻看到"对方断线了"
    }
    // 重复上报同一状态不做任何事（幂等）。
    return PresenceOutcome::kOk;
}

bool BattleRoom::ResolveReconnectGrace(std::int64_t now_ms) {
    if (phase_ != RoomPhase::kPlaying || runtime_.size() != players_.size()) {
        return false;
    }

    std::size_t offline_count = 0;
    std::size_t online_index = 0;
    std::size_t online_count = 0;
    // 最晚的断线时刻：只有它过期才说明"所有人都已经等够了"。
    std::int64_t latest_offline_at = 0;
    for (std::size_t i = 0; i < runtime_.size(); ++i) {
        if (runtime_[i].online) {
            ++online_count;
            online_index = i;
            continue;
        }
        ++offline_count;
        latest_offline_at = std::max(latest_offline_at, runtime_[i].offline_since_ms);
    }
    if (offline_count == 0) {
        return false;
    }

    // 用"最晚断线者"判断是否全体到期：有人还在自己的宽限期内就继续等。
    if (now_ms - latest_offline_at < kReconnectGraceMs) {
        return false;
    }

    if (online_count > 0) {
        // 还有人在线：判断线方负。两人房间里 online_count == 1，胜者就是那一位。
        Finish(FinishReason::kDisconnect, players_[online_index].player_id, now_ms);
        return true;
    }

    // 双方都没回来：这一局没有任何一方值得判负，作废且不写结果。
    Abort(now_ms);
    return true;
}

void BattleRoom::Abort(std::int64_t now_ms) {
    phase_ = RoomPhase::kAborted;
    finish_reason_ = FinishReason::kAborted;
    winner_id_.clear();
    finished_at_ms_ = now_ms;
    PushSnapshot();
}

void BattleRoom::MarkResultPersisted() {
    if (phase_ == RoomPhase::kFinishing) {
        phase_ = RoomPhase::kFinished;
    }
}

void BattleRoom::MarkResultFailed(std::int64_t now_ms) {
    if (phase_ == RoomPhase::kFinishing) {
        last_persist_attempt_ms_ = now_ms;
    }
}

bool BattleRoom::ShouldRetryPersist(std::int64_t now_ms) const {
    if (phase_ != RoomPhase::kFinishing) {
        return false;
    }
    return now_ms - last_persist_attempt_ms_ >= kResultRetryIntervalMs;
}

RoomSnapshot BattleRoom::Snapshot() const {
    RoomSnapshot snapshot;
    snapshot.room_id = room_id_;
    snapshot.match_id = match_id_;
    snapshot.phase = phase_;
    snapshot.frame = frame_;
    snapshot.players = players_;
    snapshot.finish_reason = finish_reason_;
    snapshot.winner_id = winner_id_;
    snapshot.started_at_ms = started_at_ms_;
    snapshot.finished_at_ms = finished_at_ms_;
    return snapshot;
}

std::optional<MatchResultRecord> BattleRoom::Result() const {
    if (phase_ != RoomPhase::kFinishing && phase_ != RoomPhase::kFinished) {
        return std::nullopt;
    }
    MatchResultRecord record;
    record.match_id = match_id_;
    record.room_id = room_id_;
    record.winner_id = winner_id_;
    record.player_count = player_count_;
    record.started_at_ms = started_at_ms_;
    record.finished_at_ms = finished_at_ms_;
    return record;
}

bool BattleRoom::IsExpired(std::int64_t now_ms) const {
    if (phase_ != RoomPhase::kFinished && phase_ != RoomPhase::kAborted) {
        return false;
    }
    return now_ms - finished_at_ms_ >= kFinishedRetentionMs;
}

void BattleRoom::PushSnapshot() {
    history_.push_back(Snapshot());
    while (history_.size() > kMaxSnapshotHistory) {
        history_.pop_front();
    }
}

}  // namespace rgbt::room
