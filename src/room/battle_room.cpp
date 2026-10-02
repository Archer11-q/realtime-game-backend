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
    player_count_ = static_cast<std::int32_t>(players_.size());
    PushSnapshot();
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
            phase_ = RoomPhase::kAborted;
            finish_reason_ = FinishReason::kAborted;
            finished_at_ms_ = now_ms;
            PushSnapshot();
            return true;
        }
        return false;
    }

    if (phase_ != RoomPhase::kPlaying) {
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
