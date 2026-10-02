#include "room_manager.hpp"

#include <cstdio>
#include <utility>

#include "common/token.hpp"

namespace rgbt::room {
namespace {

/// 默认的房间号生成器。
///
/// 复用会话 Token 的随机串生成器：同样是「服务端生成、客户端不可预测」的标识。
/// 前缀 `r-` 让日志里一眼能看出这是房间号而不是匹配号或会话 Token。
std::string GenerateRoomId() {
    return "r-" + rgbt::common::GenerateToken(24);
}

}  // namespace

RoomManager::RoomManager(MatchResultWriter* writer, RoomSnapshotWriter* snapshot_writer,
                         std::function<std::string()> room_id_factory)
    : writer_(writer),
      snapshot_writer_(snapshot_writer),
      room_id_factory_(room_id_factory ? std::move(room_id_factory) : GenerateRoomId) {}

std::string RoomManager::MakeRoomId() const {
    return room_id_factory_ ? room_id_factory_() : std::string();
}

CreateOutcome RoomManager::Create(const std::string& match_id,
                                  const std::vector<std::string>& player_ids, std::int64_t now_ms,
                                  std::string* out_room_id, RoomSnapshot* out_snapshot) {
    if (match_id.empty() || match_id.size() > kMaxIdLength || player_ids.empty()) {
        return CreateOutcome::kInvalidArgument;
    }
    for (const std::string& player_id : player_ids) {
        if (player_id.empty() || player_id.size() > kMaxIdLength) {
            return CreateOutcome::kInvalidArgument;
        }
    }

    const std::lock_guard<std::mutex> lock(mutex_);

    // 幂等：同一 match_id 重复创建返回同一房间，绝不新建第二个。
    // 这保证 Match 在调用超时后重试不会为一局造出两个房间。
    const auto existing = match_index_.find(match_id);
    if (existing != match_index_.end()) {
        const auto room = rooms_.find(existing->second);
        if (room != rooms_.end()) {
            if (out_room_id != nullptr) {
                *out_room_id = room->second->room_id();
            }
            if (out_snapshot != nullptr) {
                *out_snapshot = room->second->Snapshot();
            }
            return CreateOutcome::kOk;
        }
        // 索引指向的房间已经不在了（理论上只会在回收逻辑出错时发生）。
        // 清理残留索引后按新建处理，而不是返回一个不存在的房间号。
        match_index_.erase(existing);
    }

    std::string room_id = MakeRoomId();
    if (room_id.empty()) {
        return CreateOutcome::kInternal;
    }

    auto room = std::make_unique<BattleRoom>(room_id, match_id, player_ids, now_ms);
    if (out_snapshot != nullptr) {
        *out_snapshot = room->Snapshot();
    }
    if (out_room_id != nullptr) {
        *out_room_id = room_id;
    }
    rooms_.emplace(room_id, std::move(room));
    match_index_.emplace(match_id, room_id);
    return CreateOutcome::kOk;
}

std::optional<JoinOutcome> RoomManager::Join(const std::string& room_id,
                                             const std::string& player_id, std::int64_t now_ms,
                                             RoomSnapshot* out_snapshot) {
    const std::lock_guard<std::mutex> lock(mutex_);

    const auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return std::nullopt;
    }

    const JoinOutcome outcome = it->second->Join(player_id, now_ms);
    if (out_snapshot != nullptr) {
        *out_snapshot = it->second->Snapshot();
    }
    return outcome;
}

std::optional<SubmitOutcome> RoomManager::SubmitInput(const std::string& room_id,
                                                      const std::string& player_id, InputKind kind,
                                                      RoomSnapshot* out_snapshot) {
    const std::lock_guard<std::mutex> lock(mutex_);

    const auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return std::nullopt;
    }

    const SubmitOutcome outcome = it->second->SubmitInput(player_id, kind);
    if (out_snapshot != nullptr) {
        *out_snapshot = it->second->Snapshot();
    }
    return outcome;
}

bool RoomManager::GetState(const std::string& room_id, std::int64_t now_ms,
                           RoomSnapshot* out_snapshot) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ReapExpiredLocked(now_ms);

    const auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return false;
    }
    if (out_snapshot != nullptr) {
        *out_snapshot = it->second->Snapshot();
    }
    return true;
}

ResultOutcome RoomManager::GetResult(const std::string& match_id, std::int64_t now_ms,
                                     MatchResultRecord* out_record, RoomSnapshot* out_snapshot) {
    if (match_id.empty() || match_id.size() > kMaxIdLength || out_record == nullptr) {
        return ResultOutcome::kNotFound;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ReapExpiredLocked(now_ms);

        const auto index = match_index_.find(match_id);
        if (index != match_index_.end()) {
            const auto room = rooms_.find(index->second);
            if (room != rooms_.end()) {
                const BattleRoom& target = *room->second;
                if (out_snapshot != nullptr) {
                    *out_snapshot = target.Snapshot();
                }
                switch (target.phase()) {
                    case RoomPhase::kFinishing:
                        // 已分出胜负但结果尚未落库。**不能**返回内存中的胜负，
                        // 否则进程崩溃后客户端拿到过一个查不到的结果。
                        return ResultOutcome::kPending;
                    case RoomPhase::kAborted:
                        // 异常终止的对局不产生结果。
                        return ResultOutcome::kNotFound;
                    case RoomPhase::kFinished:
                        // 已确认落库：往下走，以数据库为准读出。
                        break;
                    case RoomPhase::kCreated:
                    case RoomPhase::kWaiting:
                    case RoomPhase::kPlaying:
                        // 对局还没结束，自然没有结果。
                        return ResultOutcome::kNotFound;
                }
            }
        }
    }

    // 走到这里有两种情况：房间已 FINISHED（读库以确认内容），
    // 或者房间不在内存中（例如进程重启后查询历史对局）。
    // 两种都以数据库为准，因此这里**不持锁**，避免一次 MySQL 超时卡住所有房间。
    if (writer_ == nullptr) {
        return ResultOutcome::kUnavailable;
    }
    switch (writer_->Read(match_id, out_record)) {
        case ReadStatus::kOk:
            return ResultOutcome::kOk;
        case ReadStatus::kNotFound:
            return ResultOutcome::kNotFound;
        case ReadStatus::kUnavailable:
        default:
            return ResultOutcome::kUnavailable;
    }
}

void RoomManager::Tick(std::int64_t now_ms) {
    std::vector<std::string> to_persist;
    std::vector<RoomSnapshotRecord> snapshots;

    {
        const std::lock_guard<std::mutex> lock(mutex_);

        for (auto& entry : rooms_) {
            entry.second->Tick(now_ms);
        }
        for (const auto& entry : rooms_) {
            if (entry.second->phase() == RoomPhase::kFinishing &&
                entry.second->ShouldRetryPersist(now_ms)) {
                to_persist.push_back(entry.first);
            }
        }

        // 收集本轮到期的房间快照。**只在这里取副本**，写库放到锁外。
        // 快照与结果落库共用同一条"锁内取、锁外写"的规则，理由相同：
        // 一次 MySQL 超时不能让所有房间的 tick 一起停摆。
        if (snapshot_writer_ != nullptr) {
            for (auto& entry : rooms_) {
                BattleRoom& room = *entry.second;
                SnapshotState& state = snapshot_state_[room.match_id()];
                if (!ShouldSnapshot(state, room.phase(), now_ms)) {
                    continue;
                }
                snapshots.push_back(MakeSnapshotRecord(room, now_ms));
                state.last_write_ms = now_ms;
                state.last_phase = room.phase();
                state.written = true;
            }
        }

        ReapExpiredLocked(now_ms);
    }

    // 快照先写。它与结果落库互不依赖，但快照是旁路、失败也无所谓，
    // 放在前面可以让"本轮有没有快照要写"这件事更早看到。
    FlushSnapshots(snapshots);

    // 持久化放在锁外：MySQL 调用可能耗时数百毫秒甚至超时，
    // 持锁会让所有房间的 tick 一起停顿。
    for (const std::string& room_id : to_persist) {
        MatchResultRecord record;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto it = rooms_.find(room_id);
            if (it == rooms_.end() || it->second->phase() != RoomPhase::kFinishing) {
                continue;
            }
            const std::optional<MatchResultRecord> pending = it->second->Result();
            if (!pending.has_value()) {
                continue;
            }
            record = *pending;
        }

        const WriteStatus status =
            writer_ != nullptr ? writer_->Write(record) : WriteStatus::kUnavailable;

        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            // 房间在写入期间被回收，说明它已经不需要这个结果了。
            continue;
        }
        if (status == WriteStatus::kOk) {
            it->second->MarkResultPersisted();
        } else {
            // 不编造成功：房间停在 FINISHING，下一次 Tick 继续重试。
            // 这条日志是「存储一直不可用」的唯一早期信号。
            std::fprintf(stderr, "[room] 对局结果写入失败，将在 %lld ms 后重试：match_id=%s\n",
                         static_cast<long long>(kResultRetryIntervalMs), record.match_id.c_str());
            it->second->MarkResultFailed(now_ms);
        }
    }
}

void RoomManager::ReapExpiredLocked(std::int64_t now_ms) {
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        if (!it->second->IsExpired(now_ms)) {
            ++it;
            continue;
        }
        match_index_.erase(it->second->match_id());
        // 快照调度状态必须跟着房间一起清掉，否则这张表会随对局数无限增长，
        // 而且残留的状态会让同 match_id 的后续房间以为"已经写过快照"。
        snapshot_state_.erase(it->second->match_id());
        it = rooms_.erase(it);
    }
}

bool RoomManager::ShouldSnapshot(const SnapshotState& state, RoomPhase phase, std::int64_t now_ms) {
    const bool terminal = (phase == RoomPhase::kFinished || phase == RoomPhase::kAborted);
    if (terminal && (!state.written || state.last_phase != phase)) {
        // 进入终态时立刻写一次，别让快照停在中间态（例如结果已落库但表里还是
        // FINISHING）。判据是"阶段变了"而不是"隔了多久"，因为这里不该有延迟。
        return true;
    }
    if (!state.written) {
        // 第一个快照立刻写：让"这里曾经有一局"从一开始就成立。
        return true;
    }
    return now_ms - state.last_write_ms >= kSnapshotIntervalMs;
}

RoomSnapshotRecord RoomManager::MakeSnapshotRecord(const BattleRoom& room, std::int64_t now_ms) {
    const RoomSnapshot snapshot = room.Snapshot();
    RoomSnapshotRecord record;
    record.match_id = snapshot.match_id;
    record.room_id = snapshot.room_id;
    record.phase = snapshot.phase;
    record.frame = snapshot.frame;
    record.winner_id = snapshot.winner_id;
    record.finish_reason = snapshot.finish_reason;
    record.started_at_ms = snapshot.started_at_ms;
    record.finished_at_ms = snapshot.finished_at_ms;
    record.snapshot_at_ms = now_ms;

    // 定长数组与固定两人一局一一对应。房间的玩家数由 BattleRoom 的不变量保证，
    // 这里再夹一次下标，是为了即便上游出错也不会越界写内存。
    for (std::size_t i = 0; i < kPlayersPerRoom && i < snapshot.players.size(); ++i) {
        record.players[i].player_id = snapshot.players[i].player_id;
        record.players[i].hp = snapshot.players[i].hp;
        // connected 表达的是"已在房间内"（见 room_types.hpp 的说明），
        // 正好就是 rooms.pN_joined 要存的东西。
        record.players[i].joined = snapshot.players[i].connected;
    }
    return record;
}

void RoomManager::FlushSnapshots(const std::vector<RoomSnapshotRecord>& records) {
    if (snapshot_writer_ == nullptr || records.empty()) {
        return;
    }

    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    for (const RoomSnapshotRecord& record : records) {
        if (snapshot_writer_->Save(record) == SnapshotWriteStatus::kOk) {
            ++succeeded;
            continue;
        }
        ++failed;
        // **故意不重试**：快照可以丢弃，下一个周期会天然覆盖它。
        // 把它做成"必须成功"会让一次 MySQL 抖动在内存里堆起一批过期快照，
        // 而收益为零——过期快照本来就没有价值。这是它与对局结果的根本区别。
        //
        // 失败原因由 writer 记录（只有它知道 MySQL 的错误），这里只计数，
        // 避免同一个失败被记两遍、把日志刷满。
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_write_count_ += succeeded;
    snapshot_failure_count_ += failed;
}

std::uint64_t RoomManager::SnapshotWriteCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_write_count_;
}

std::uint64_t RoomManager::SnapshotFailureCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_failure_count_;
}

std::size_t RoomManager::RoomCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return rooms_.size();
}

std::size_t RoomManager::PlayingCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : rooms_) {
        if (entry.second->phase() == RoomPhase::kPlaying) {
            ++count;
        }
    }
    return count;
}

std::size_t RoomManager::PendingResultCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : rooms_) {
        if (entry.second->phase() == RoomPhase::kFinishing) {
            ++count;
        }
    }
    return count;
}

}  // namespace rgbt::room
