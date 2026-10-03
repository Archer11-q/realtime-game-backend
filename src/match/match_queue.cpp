#include "match_queue.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "common/logging.hpp"
#include "common/token.hpp"

namespace rgbt::match {
namespace {

/// 默认的匹配 ID 生成器。
///
/// 复用会话 Token 的随机串生成器：同样是「服务端生成、客户端不可预测」的标识。
/// 前缀 `m-` 让日志和 Redis Key 里一眼能看出这是匹配 ID 而不是会话 Token。
std::string GenerateMatchId() {
    return "m-" + rgbt::common::GenerateToken(24);
}

}  // namespace

MatchQueue::MatchQueue(RoomAllocator* allocator, std::function<std::string()> match_id_factory,
                       std::int64_t match_timeout_ms, std::int64_t result_ttl_ms,
                       std::size_t max_queue_size, MatchQueueStore* store)
    : allocator_(allocator),
      store_(store),
      match_id_factory_(match_id_factory ? std::move(match_id_factory) : GenerateMatchId),
      match_timeout_ms_(match_timeout_ms > 0 ? match_timeout_ms : kDefaultMatchTimeoutMs),
      result_ttl_ms_(result_ttl_ms > 0 ? result_ttl_ms : kDefaultResultTtlMs),
      max_queue_size_(max_queue_size > 0 ? max_queue_size : kDefaultMaxQueueSize) {}

MatchRestoreReport MatchQueue::Restore(std::int64_t now_ms) {
    MatchRestoreReport report;
    if (store_ == nullptr) {
        // 没配存储：不恢复，也不报错。单元测试与"不关心持久化"的部署会走这里。
        return report;
    }

    std::vector<MatchQueueSnapshotRow> rows;
    if (!store_->Load(&rows)) {
        // 存储不可用时**一个条目都不恢复**，并如实报告 load_failed。
        // 空手启动是安全的：排队中的客户端会重新入队。而"以为恢复了其实没有"
        // 会让玩家在队列里等一个永远不会被配上的号。
        report.load_failed = true;
        rgbt::common::LogWarn("queue_snapshot_load_failed",
                              {{"consequence", "本次启动不恢复任何排队状态"}});
        return report;
    }
    report.scanned = rows.size();

    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MatchQueueEntry> queued;
    std::vector<MatchQueueEntry> matched;
    for (const MatchQueueSnapshotRow& row : rows) {
        if (!row.problem.empty()) {
            ++report.dropped;
            rgbt::common::LogWarn("queue_snapshot_entry_skipped",
                                  {{"reason", row.problem}, {"policy", "跳过而不静默丢弃"}});
            continue;
        }
        if (row.entry.kind == SnapshotEntryKind::kQueued) {
            queued.push_back(row.entry);
        } else {
            matched.push_back(row.entry);
        }
    }

    // 按入队时刻**稳定**排序重建 FIFO：同一毫秒入队的两人必须保持原先后。
    std::stable_sort(queued.begin(), queued.end(),
                     [](const MatchQueueEntry& lhs, const MatchQueueEntry& rhs) {
                         return lhs.queued_at_ms < rhs.queued_at_ms;
                     });

    for (const MatchQueueEntry& entry : queued) {
        if (entries_.count(entry.player_id) != 0U) {
            // 同一玩家出现两次（正常写入不会产生）。保留先出现的那条并计数，
            // 而不是让后者覆盖前者——覆盖会让顺序变得难以解释。
            ++report.dropped;
            rgbt::common::LogWarn("queue_snapshot_duplicate_player",
                                  {{"player", entry.player_id}, {"action", "跳过后一条"}});
            continue;
        }
        if (now_ms - entry.queued_at_ms >= match_timeout_ms_) {
            // **已经超时的排队条目不再恢复**（停机时间算在等待里）。
            //
            // 为什么不恢复成 kTimeout 状态：快照可能来自很久以前的一次启动，
            // 把这些条目装进内存会让"一个从没在这个进程里排过队的人"立刻报告
            // `timeout` 并占着结果保留期不放。玩家该做的就是重新入队，
            // 而"队列里没有他"正是这个语义最直接的表达。
            ++report.timed_out;
            continue;
        }
        Entry restored;
        restored.state = MatchStatusSnapshot::State::kQueued;
        restored.queued_at_ms = entry.queued_at_ms;
        restored.stamp_ms = entry.queued_at_ms;
        entries_.emplace(entry.player_id, std::move(restored));
        queue_.push_back(entry.player_id);
        ++report.queued;
    }

    for (const MatchQueueEntry& entry : matched) {
        if (now_ms - entry.stamp_ms >= result_ttl_ms_) {
            // 同理：结果保留期已过的对局不再恢复。玩家会以空闲状态重新入队。
            report.expired_results += entry.player_ids.size();
            continue;
        }
        bool already_seen = false;
        for (const std::string& player_id : entry.player_ids) {
            if (entries_.count(player_id) != 0U) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) {
            ++report.dropped;
            rgbt::common::LogWarn("queue_snapshot_duplicate_result",
                                  {{"match_id", entry.match_id}, {"action", "跳过"}});
            continue;
        }
        // 一条 matched 记录服务这一局的所有玩家：entries_ 以 player_id 为键，
        // 每个玩家都要有自己的条目，否则另一个玩家会被当成"从没排过队"。
        for (const std::string& player_id : entry.player_ids) {
            Entry restored;
            restored.state = MatchStatusSnapshot::State::kMatched;
            restored.match_id = entry.match_id;
            restored.room_id = entry.room_id;
            restored.player_ids = entry.player_ids;
            restored.stamp_ms = entry.stamp_ms;
            entries_.emplace(player_id, std::move(restored));
        }
        report.matched += entry.player_ids.size();
    }

    // 恢复时刻已经没有"待清算"的东西了：上面按超时与结果保留期逐条过滤过。
    // 保留这次调用只是为了让"重启后内存里不存在已过期条目"这条不变量有个兜底。
    (void)SweepLocked(now_ms);
    return report;
}

MatchQueueSnapshot MatchQueue::MakeSnapshotLocked() const {
    MatchQueueSnapshot snapshot;
    snapshot.queued.reserve(queue_.size() + entries_.size());

    // 1. 排队中的玩家，按队列顺序（FIFO）。
    for (const std::string& player_id : queue_) {
        const auto it = entries_.find(player_id);
        if (it == entries_.end() || it->second.state != MatchStatusSnapshot::State::kQueued) {
            continue;  // 防御：队列里只应放排队中的玩家
        }
        MatchQueueEntry entry;
        entry.kind = SnapshotEntryKind::kQueued;
        entry.player_id = player_id;
        entry.queued_at_ms = it->second.queued_at_ms;
        snapshot.queued.push_back(std::move(entry));
    }

    // 2. 「正在分配房间」的玩家：他们已从 queue_ 移出，但对外仍是排队中。
    //    作为排队条目写入，重启后退回队列（TASK-015 决策 4）。
    //
    //    已经登记取消意图的**不写**：客户端的取消已经返回成功，重启后让它重新
    //    出现在队列里会与那个答复矛盾。写不进去的代价也仅仅是这一局作废。
    for (const auto& pair : entries_) {
        if (pair.second.state != MatchStatusSnapshot::State::kQueued || !pair.second.allocating) {
            continue;
        }
        if (pair.second.cancel_requested) {
            continue;
        }
        MatchQueueEntry entry;
        entry.kind = SnapshotEntryKind::kQueued;
        entry.player_id = pair.first;
        entry.queued_at_ms = pair.second.queued_at_ms;
        snapshot.queued.push_back(std::move(entry));
    }

    // 3. 已配对的结果。entries_ 以 player_id 为键，因此同一个 match_id 会出现
    //    多次，这里按 match_id 去重，一局只写一条。
    std::vector<std::string> written_matches;
    for (const auto& pair : entries_) {
        const Entry& entry = pair.second;
        if (entry.state != MatchStatusSnapshot::State::kMatched) {
            continue;
        }
        if (std::find(written_matches.begin(), written_matches.end(), entry.match_id) !=
            written_matches.end()) {
            continue;
        }
        MatchQueueEntry record;
        record.kind = SnapshotEntryKind::kMatched;
        record.match_id = entry.match_id;
        record.room_id = entry.room_id;
        record.player_ids = entry.player_ids;
        record.stamp_ms = entry.stamp_ms;
        snapshot.matched.push_back(std::move(record));
        written_matches.push_back(entry.match_id);
    }

    return snapshot;
}

void MatchQueue::PersistSnapshot() {
    if (store_ == nullptr) {
        return;
    }

    // 锁序：snapshot_order_mutex_ -> mutex_。先拿前者，让"取快照 + 写快照"成为一个
    // 整体，从而保证写入顺序与快照的新旧顺序一致（见头文件的说明）。
    const std::lock_guard<std::mutex> order_lock(snapshot_order_mutex_);
    MatchQueueSnapshot snapshot;
    {
        // 只在取副本时持队列锁，**不持锁做 I/O**。
        const std::lock_guard<std::mutex> lock(mutex_);
        snapshot = MakeSnapshotLocked();
    }

    if (!store_->Save(snapshot)) {
        // 快照可以丢弃：不重试、不阻塞。Redis 不可用时匹配照常工作，
        // 只是失去"重启可恢复"——这是项目所有者确认过的降级策略。
        rgbt::common::LogWarn("queue_snapshot_write_failed",
                              {{"queued", std::to_string(snapshot.queued.size())},
                               {"matched", std::to_string(snapshot.matched.size())},
                               {"policy", "可丢弃、不重试，当前降级为纯内存"}});
    }
}

EnqueueOutcome MatchQueue::Enqueue(const std::string& player_id, const std::string& request_id,
                                   std::int64_t now_ms) {
    if (player_id.empty() || player_id.size() > kMaxPlayerIdLength || request_id.empty() ||
        request_id.size() > kMaxRequestIdLength) {
        return EnqueueOutcome::kInvalidArgument;
    }

    EnqueueOutcome outcome = EnqueueOutcome::kQueued;
    bool state_changed = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_changed = SweepLocked(now_ms);

        const auto existing = entries_.find(player_id);
        if (existing != entries_.end()) {
            // 排队中、正在分配房间、或已有未领取的结果：一律按幂等处理，不新建条目。
            // 后者尤其重要——覆盖匹配结果会让玩家丢掉已经配好的局。
            if (existing->second.state == MatchStatusSnapshot::State::kQueued ||
                existing->second.state == MatchStatusSnapshot::State::kMatched) {
                outcome = EnqueueOutcome::kAlreadyQueued;
            } else if (queue_.size() >= max_queue_size_) {
                // 超时状态允许重新排队，但仍要受队列上限约束。
                outcome = EnqueueOutcome::kQueueFull;
            } else {
                // 超时状态允许重新排队：复用条目并刷新排队时刻。
                existing->second.state = MatchStatusSnapshot::State::kQueued;
                existing->second.request_id = request_id;
                existing->second.queued_at_ms = now_ms;
                existing->second.stamp_ms = now_ms;
                existing->second.allocating = false;
                existing->second.cancel_requested = false;
                queue_.push_back(player_id);
                state_changed = true;
            }
        } else if (queue_.size() >= max_queue_size_) {
            outcome = EnqueueOutcome::kQueueFull;
        } else {
            Entry entry;
            entry.state = MatchStatusSnapshot::State::kQueued;
            entry.request_id = request_id;
            entry.queued_at_ms = now_ms;
            entry.stamp_ms = now_ms;
            entries_.emplace(player_id, std::move(entry));
            queue_.push_back(player_id);
            state_changed = true;
        }
    }

    // 阶段一（锁内取人）-> 阶段二（锁外分配）-> 阶段三（锁内提交），见
    // RunPairingRound 的注释。
    const bool paired = RunPairingRound(now_ms);

    // 一次写入覆盖本轮的全部变化（入队 / 清算 / 配对），避免同一请求写多份快照。
    if (state_changed || paired) {
        PersistSnapshot();
    }
    return outcome;
}

bool MatchQueue::RunPairingRound(std::int64_t now_ms) {
    std::vector<PendingGroup> groups;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // 阶段一：只取人、生成 match_id、标记分配中，不做任何远程调用。
        groups = TakePairGroupsLocked();
    }
    if (groups.empty()) {
        return false;
    }

    // 阶段二：在**锁外**分配房间。这一步会发起 brpc 调用，持锁会让整个队列停顿。
    //
    // 阶段三：回到锁内提交。分配失败时把玩家放回队首等下一次配对，因此「Room 不可用」
    // 不会让玩家被莫名标记为超时，也不会产生半成品匹配。
    for (const PendingGroup& group : groups) {
        std::string room_id;
        if (allocator_ != nullptr) {
            room_id = allocator_->Allocate(group.match_id, group.player_ids);
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        CommitGroupLocked(group, room_id, now_ms);
    }
    return true;
}

bool MatchQueue::RetryPairingIfNeeded(std::int64_t now_ms) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!retry_pairing_) {
            return false;
        }
        // 先清掉标记再重试：重试若再失败，CommitGroupLocked 会重新置上。
        retry_pairing_ = false;
    }
    return RunPairingRound(now_ms);
}

bool MatchQueue::Cancel(const std::string& player_id, std::int64_t now_ms) {
    bool removed = false;
    bool state_changed = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_changed = SweepLocked(now_ms);

        const auto it = entries_.find(player_id);
        if (it == entries_.end()) {
            // 落空：仍要把本次清算的结果写回快照，所以不能直接 return。
        } else if (it->second.allocating) {
            // 正在分配房间。此刻不能直接删除条目——阶段三还需要它来判断这一局是否成立。
            // 只登记取消意图，由 CommitGroupLocked 落实「不把取消的人塞进对局」。
            it->second.cancel_requested = true;
            removed = true;
            state_changed = true;
        } else if (it->second.state != MatchStatusSnapshot::State::kQueued) {
            // 不在队列中（已被配对 / 已超时 / 已取消）都按幂等成功处理。
        } else {
            entries_.erase(it);
            RemoveFromQueueLocked(player_id);
            removed = true;
            state_changed = true;
        }
    }

    if (state_changed) {
        PersistSnapshot();
    }
    return removed;
}

MatchStatusSnapshot MatchQueue::GetStatus(const std::string& player_id, std::int64_t now_ms) {
    MatchStatusSnapshot snapshot;
    bool state_changed = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_changed = SweepLocked(now_ms);
        snapshot = SnapshotLocked(player_id);
    }

    // 惰性重试：上一次房间分配失败时玩家已退回队列。客户端会持续轮询本接口，
    // 因此在这里顺带重试一次配对，玩家不需要重新入队。
    //
    // 返回的是重试**之前**的快照：这一次调用可能把玩家从 queued 变成 matched，
    // 但那是本次调用产生的新事实，下一次轮询才应该看到。假装它已经发生会让调用方
    // 拿到一个与本次请求无关的状态。
    const bool paired = RetryPairingIfNeeded(now_ms);

    // 清算（超时淘汰、结果过期）与配对都会改变状态，因此这两条路径都要写快照。
    // 反过来说：仅仅"查了一次状态"不会产生写入，轮询不会变成写放大。
    if (state_changed || paired) {
        PersistSnapshot();
    }
    return snapshot;
}

std::size_t MatchQueue::QueueSize() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t MatchQueue::AllocatingCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : entries_) {
        if (entry.second.allocating) {
            ++count;
        }
    }
    return count;
}

bool MatchQueue::SweepLocked(std::int64_t now_ms) {
    bool changed = false;

    // 1. 淘汰排队超时的玩家。分配中的玩家已不在 queue_ 中，因此不会被误淘汰。
    if (!queue_.empty()) {
        std::vector<std::string> timed_out;
        for (const std::string& player_id : queue_) {
            const auto it = entries_.find(player_id);
            if (it == entries_.end() || it->second.state != MatchStatusSnapshot::State::kQueued) {
                // 理论上不该出现：队列里只放排队中的玩家。作为防御保留。
                timed_out.push_back(player_id);
                continue;
            }
            if (now_ms - it->second.queued_at_ms >= match_timeout_ms_) {
                timed_out.push_back(player_id);
            }
        }
        for (const std::string& player_id : timed_out) {
            auto it = entries_.find(player_id);
            if (it != entries_.end()) {
                it->second.state = MatchStatusSnapshot::State::kTimeout;
                it->second.stamp_ms = now_ms;
            }
            RemoveFromQueueLocked(player_id);
            changed = true;
        }
    }

    // 2. 清理过期结果。过期后玩家回到「无记录」，即 kIdle，需要重新入队。
    for (auto it = entries_.begin(); it != entries_.end();) {
        const MatchStatusSnapshot::State state = it->second.state;
        const bool is_result = state == MatchStatusSnapshot::State::kMatched ||
                               state == MatchStatusSnapshot::State::kTimeout;
        if (!is_result) {
            ++it;
            continue;
        }
        if (now_ms - it->second.stamp_ms >= result_ttl_ms_) {
            it = entries_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    return changed;
}

std::vector<MatchQueue::PendingGroup> MatchQueue::TakePairGroupsLocked() {
    std::vector<PendingGroup> groups;

    while (queue_.size() >= kPlayersPerMatch) {
        std::vector<std::string> group;
        group.reserve(kPlayersPerMatch);

        // 从队首取人。队列中只会有仍然排队的玩家（取消与超时都会即时移除），
        // 因此这里不需要重复校验状态。
        while (group.size() < kPlayersPerMatch && !queue_.empty()) {
            std::string player_id = queue_.front();
            queue_.pop_front();
            const auto it = entries_.find(player_id);
            if (it == entries_.end() || it->second.state != MatchStatusSnapshot::State::kQueued) {
                continue;  // 防御：状态不一致的人不参与配对，也不放回队列
            }
            group.push_back(std::move(player_id));
        }

        if (group.size() < kPlayersPerMatch) {
            // 取不满两人，说明队列已被清空（防御路径）。把已取出的人放回队首，
            // 保持 FIFO 顺序，等待下一个人入队。
            for (auto it = group.rbegin(); it != group.rend(); ++it) {
                queue_.push_front(*it);
            }
            break;
        }

        const std::string match_id = match_id_factory_();
        if (match_id.empty()) {
            // 生成失败：放回队首，等下一次入队再试。不把玩家标成超时——
            // 这不是玩家的错，也不该让他们重新排队。
            for (auto it = group.rbegin(); it != group.rend(); ++it) {
                queue_.push_front(*it);
            }
            break;
        }

        PendingGroup pending;
        pending.match_id = match_id;
        pending.player_ids = group;

        for (const std::string& player_id : group) {
            auto it = entries_.find(player_id);
            if (it == entries_.end()) {
                continue;
            }
            // 状态仍保持 kQueued：分配期间客户端看到的语义不变。
            it->second.allocating = true;
            it->second.match_id = match_id;
            it->second.player_ids = group;
        }
        groups.push_back(std::move(pending));
    }

    return groups;
}

void MatchQueue::CommitGroupLocked(const PendingGroup& group, const std::string& room_id,
                                   std::int64_t now_ms) {
    // 先把这一组的玩家分类：取消的、以及需要退回队列的。
    std::vector<std::string> cancelled;
    std::vector<std::string> requeue;
    for (const std::string& player_id : group.player_ids) {
        const auto it = entries_.find(player_id);
        if (it == entries_.end()) {
            continue;
        }
        if (it->second.cancel_requested) {
            cancelled.push_back(player_id);
        } else {
            requeue.push_back(player_id);
        }
    }

    const bool group_viable = room_id.empty() ? false : cancelled.empty();

    if (group_viable) {
        for (const std::string& player_id : requeue) {
            auto it = entries_.find(player_id);
            if (it == entries_.end()) {
                continue;
            }
            it->second.state = MatchStatusSnapshot::State::kMatched;
            it->second.allocating = false;
            it->second.room_id = room_id;
            it->second.player_ids = group.player_ids;
            it->second.stamp_ms = now_ms;
        }
        return;
    }

    // 走到这里有两种可能：
    //   1. 房间分配失败（room_id 为空）——所有人退回队列。
    //   2. 分配成功但有人在这期间取消了——这一局不成立，剩余的人退回队列。
    //
    // 第 2 种情况下，Room 侧已经创建了房间。这里**不回收它**：Room 的「等待玩家加入
    // 超时」会在 kWaitingTimeoutMs 之后把无人加入的房间标记为 ABORTED 并回收。
    // 让空闲房间自己过期，比在两个服务之间加一次反向删除调用更简单可靠。
    if (room_id.empty()) {
        // 记下「有失败的分配合并待重试」。配对原本只在有人入队时触发，
        // 不记这个标记的话，退回队列的玩家要等到下一个新玩家出现才可能被配上。
        // Room 重启后 brpc channel 惰性重连，第一次调用必然失败，这个窗口很容易被撞上。
        retry_pairing_ = true;
    }

    for (const std::string& player_id : cancelled) {
        entries_.erase(player_id);
    }

    // 退回队首而不是队尾：这些人比新来的人等待更久，排到队尾会无限延长等待。
    // 逆序 push_front 才能保持原来的相对顺序。
    for (auto it = requeue.rbegin(); it != requeue.rend(); ++it) {
        auto entry = entries_.find(*it);
        if (entry == entries_.end()) {
            continue;
        }
        entry->second.allocating = false;
        entry->second.match_id.clear();
        entry->second.player_ids.clear();
        queue_.push_front(*it);
    }
}

void MatchQueue::RemoveFromQueueLocked(const std::string& player_id) {
    const auto it = std::find(queue_.begin(), queue_.end(), player_id);
    if (it != queue_.end()) {
        queue_.erase(it);
    }
}

MatchStatusSnapshot MatchQueue::SnapshotLocked(const std::string& player_id) const {
    MatchStatusSnapshot snapshot;
    snapshot.queue_size = queue_.size();

    const auto it = entries_.find(player_id);
    if (it == entries_.end()) {
        return snapshot;  // kIdle
    }

    snapshot.state = it->second.state;
    snapshot.request_id = it->second.request_id;
    snapshot.match_id = it->second.match_id;
    snapshot.room_id = it->second.room_id;
    snapshot.player_ids = it->second.player_ids;
    snapshot.queued_at_ms = it->second.queued_at_ms;
    return snapshot;
}

}  // namespace rgbt::match
