#include "room_manager.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
                         RoomSnapshotReader* snapshot_reader,
                         std::function<std::string()> room_id_factory)
    : writer_(writer),
      snapshot_writer_(snapshot_writer),
      snapshot_reader_(snapshot_reader),
      room_id_factory_(room_id_factory ? std::move(room_id_factory) : GenerateRoomId) {}

RestoreReport RoomManager::Restore(std::int64_t now_ms) {
    RestoreReport report;
    if (snapshot_reader_ == nullptr) {
        // 没配读取器：不恢复，也不报错。单元测试与"不关心持久化"的部署会走这里。
        return report;
    }

    std::vector<RoomSnapshotRow> rows;
    if (!snapshot_reader_->LoadUnfinished(&rows)) {
        // 存储不可用时**不恢复任何房间**，并如实报告 load_failed。
        // 空手启动是安全的：玩家发现房间没了会重新匹配。
        // 而"以为恢复了其实没有"会让后续每次查询都得到难以解释的结果。
        report.load_failed = true;
        std::fprintf(stderr, "[room] 房间快照读取失败：本次启动不恢复任何房间\n");
        return report;
    }
    report.scanned = rows.size();

    std::vector<std::pair<RoomSnapshotRecord, std::string>> rejected;
    // 成功重建的房间也要留一份记录，好在锁外打日志。**不能在持锁循环里打**：
    // 持锁做 I/O 是本项目明确避免的（见 room_manager.hpp 的并发说明）。
    std::vector<RoomSnapshotRecord> restored_records;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const RoomSnapshotRow& row : rows) {
            // 解析层与校验层的问题都汇聚在 problem 里，这里只认"非空即拒绝"。
            if (!row.problem.empty()) {
                rejected.emplace_back(row.record, row.problem);
                continue;
            }
            std::optional<BattleRoom> room = BattleRoom::RestoreFrom(row.record, now_ms);
            if (!room.has_value()) {
                rejected.emplace_back(row.record, "无法由该快照重建房间");
                continue;
            }
            const std::string room_id = room->room_id();
            const std::string match_id = room->match_id();
            // 幂等：内存里已有同一房间或同一对局时保留内存里的那份。
            // Restore 只在启动时调用一次，这里防御的是"调用方重复调用"。
            if (rooms_.count(room_id) != 0U || match_index_.count(match_id) != 0U) {
                continue;
            }
            rooms_.emplace(room_id, std::make_unique<BattleRoom>(std::move(*room)));
            match_index_.emplace(match_id, room_id);
            restored_records.push_back(row.record);
            ++report.restored;
        }
    }
    report.rejected = rejected.size();

    // 每个恢复出来的房间各打一行，带上**恢复点帧号**。
    //
    // 为什么必须有这条日志：房间一恢复就继续按 10 Hz 推进，因此重启后从 Gateway
    // 查询到的帧号**已经往前走了**，"恢复点究竟是哪一帧"在外部观测不到。
    // 验收脚本（scripts/verify-persistence.sh 第 7 节）靠这一行断言
    // "恢复位置精确等于最后一次快照"，否则那条断言只能退化成"大致对得上"。
    for (const RoomSnapshotRecord& record : restored_records) {
        std::fprintf(stderr, "[room] 已恢复房间：match_id=%s room_id=%s frame=%lld phase=%s\n",
                     record.match_id.c_str(), record.room_id.c_str(),
                     static_cast<long long>(record.frame), ToString(record.phase));
    }

    // 日志与写回都在锁外：持锁做 I/O 是本项目明确避免的。
    RejectSnapshots(rejected, now_ms);
    return report;
}

void RoomManager::RejectSnapshots(
    const std::vector<std::pair<RoomSnapshotRecord, std::string>>& rejected, std::int64_t now_ms) {
    if (rejected.empty()) {
        return;
    }
    for (const auto& entry : rejected) {
        std::fprintf(stderr,
                     "[room] 房间快照不可用，标记为 ABORTED（不静默丢弃）："
                     "match_id=%s room_id=%s 原因=%s\n",
                     entry.first.match_id.c_str(), entry.first.room_id.c_str(),
                     entry.second.c_str());
    }
    if (snapshot_writer_ == nullptr) {
        // 没有写入器：只记日志，**不假装写回去了**。
        std::fprintf(stderr, "[room] 未配置快照写入器，被拒绝的快照只记日志\n");
        return;
    }

    for (const auto& entry : rejected) {
        RoomSnapshotRecord record = entry.first;
        record.phase = RoomPhase::kAborted;
        record.finish_reason = FinishReason::kAborted;
        record.winner_id.clear();
        // snapshot_at_ms 必须用当前时刻：写入器靠"新不旧于旧"防止迟到的旧快照
        // 覆盖新快照，沿用原值会被它拒掉，于是这一行永远改不成终态、
        // 下次启动又被扫出来、又被拒绝。
        record.snapshot_at_ms = now_ms;
        if (snapshot_writer_->Save(record) != SnapshotWriteStatus::kOk) {
            std::fprintf(stderr,
                         "[room] 标记 ABORTED 的写回失败：match_id=%s（下次启动会再次扫到它）\n",
                         record.match_id.c_str());
        }
    }
}

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

std::optional<PresenceOutcome> RoomManager::SetPresence(const std::string& room_id,
                                                        const std::string& player_id, bool online,
                                                        std::int64_t now_ms,
                                                        RoomSnapshot* out_snapshot) {
    const std::lock_guard<std::mutex> lock(mutex_);

    const auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return std::nullopt;
    }

    const PresenceOutcome outcome = it->second->SetPresence(player_id, online, now_ms);
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

SnapshotRange RoomManager::GetSnapshotsSince(const std::string& room_id, std::int64_t since_frame,
                                             std::int64_t now_ms) {
    SnapshotRange result;

    const std::lock_guard<std::mutex> lock(mutex_);
    ReapExpiredLocked(now_ms);

    const auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        result.outcome = SnapshotRangeOutcome::kNotFound;
        return result;
    }

    const BattleRoom& room = *it->second;
    result.snapshots = room.SnapshotsAfter(since_frame);
    result.latest_frame = room.frame();
    // 窗口起点直接取缓冲里**实际存在的最小帧号**，不做任何计数推算。
    //
    // 这里原先写的是 `oldest = latest - (latest - MaxSnapshotFrame()) + 1`，等价于
    // `MaxSnapshotFrame() + 1`。那个式子在**缓冲已满**时才碰巧成立；缓冲没满时
    // `MaxSnapshotFrame() == latest`，于是算出"窗口为空"，把任何能补齐的请求都判成
    // kIncomplete（单元测试直接抓到）。条数、容量、最新帧三者都不能单独推出窗口起点，
    // 因为同一帧可能有多条记录（加入房间、断线上报、结束都会立刻写一条）。
    const BattleRoom::FrameRange window = room.SnapshotFrameRange();
    result.oldest_frame = window.oldest;

    if (since_frame > room.frame()) {
        // 客户端声称的帧**晚于**服务端当前帧。帧号由服务端单调递增，因此这只可能
        // 是客户端状态与服务端不一致，不能按"没有缺失"处理——那会让它永远停在
        // 一个服务端认为"已经最新"、实际对不上的状态上。
        //
        // 判据必须是 `>` 而不是 `>=`：相等表示"客户端与服务端停在同一帧"（对局刚
        // 建好、或双方都还没推进时的常态），那是**完全一致**，不是超前。
        // 写成 `>=` 会把"刚进房、谁都还没动"的首次订阅判成 kAhead 并回 stream.reset
        // （单元测试 SnapshotsSinceZeroOnEmptyBufferIsReady 抓到）。
        result.outcome = SnapshotRangeOutcome::kAhead;
        return result;
    }

    const std::int64_t first_needed = since_frame + 1;
    if (first_needed < result.oldest_frame) {
        // 需要的起点早于缓冲里实际保留的最早一帧：中间那段已经不在内存里了。
        // 仍返回已取到的部分（调用方可以据此判断缺口），但状态如实标为"不完整"。
        result.outcome = SnapshotRangeOutcome::kIncomplete;
        return result;
    }

    result.outcome = SnapshotRangeOutcome::kOk;
    return result;
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
