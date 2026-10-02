#include "match_queue.hpp"

#include <algorithm>
#include <utility>

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
                       std::size_t max_queue_size)
    : allocator_(allocator),
      match_id_factory_(match_id_factory ? std::move(match_id_factory) : GenerateMatchId),
      match_timeout_ms_(match_timeout_ms > 0 ? match_timeout_ms : kDefaultMatchTimeoutMs),
      result_ttl_ms_(result_ttl_ms > 0 ? result_ttl_ms : kDefaultResultTtlMs),
      max_queue_size_(max_queue_size > 0 ? max_queue_size : kDefaultMaxQueueSize) {}

EnqueueOutcome MatchQueue::Enqueue(const std::string& player_id, const std::string& request_id,
                                   std::int64_t now_ms) {
    if (player_id.empty() || player_id.size() > kMaxPlayerIdLength || request_id.empty() ||
        request_id.size() > kMaxRequestIdLength) {
        return EnqueueOutcome::kInvalidArgument;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        SweepLocked(now_ms);

        const auto existing = entries_.find(player_id);
        if (existing != entries_.end()) {
            // 排队中、正在分配房间、或已有未领取的结果：一律按幂等处理，不新建条目。
            // 后者尤其重要——覆盖匹配结果会让玩家丢掉已经配好的局。
            if (existing->second.state == MatchStatusSnapshot::State::kQueued ||
                existing->second.state == MatchStatusSnapshot::State::kMatched) {
                return EnqueueOutcome::kAlreadyQueued;
            }
            // 超时状态允许重新排队：旧的超时记录被覆盖。
            entries_.erase(existing);
        }

        if (queue_.size() >= max_queue_size_) {
            return EnqueueOutcome::kQueueFull;
        }

        Entry entry;
        entry.state = MatchStatusSnapshot::State::kQueued;
        entry.request_id = request_id;
        entry.queued_at_ms = now_ms;
        entry.stamp_ms = now_ms;
        entries_.emplace(player_id, std::move(entry));
        queue_.push_back(player_id);
    }

    // 阶段一（锁内取人）-> 阶段二（锁外分配）-> 阶段三（锁内提交），见
    // RunPairingRound 的注释。
    RunPairingRound(now_ms);

    return EnqueueOutcome::kQueued;
}

void MatchQueue::RunPairingRound(std::int64_t now_ms) {
    std::vector<PendingGroup> groups;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // 阶段一：只取人、生成 match_id、标记分配中，不做任何远程调用。
        groups = TakePairGroupsLocked();
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
}

void MatchQueue::RetryPairingIfNeeded(std::int64_t now_ms) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!retry_pairing_) {
            return;
        }
        // 先清掉标记再重试：重试若再失败，CommitGroupLocked 会重新置上。
        retry_pairing_ = false;
    }
    RunPairingRound(now_ms);
}

bool MatchQueue::Cancel(const std::string& player_id, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    SweepLocked(now_ms);

    const auto it = entries_.find(player_id);
    if (it == entries_.end()) {
        return false;
    }

    if (it->second.allocating) {
        // 正在分配房间。此刻不能直接删除条目——阶段三还需要它来判断这一局是否成立。
        // 只登记取消意图，由 CommitGroupLocked 落实「不把取消的人塞进对局」。
        it->second.cancel_requested = true;
        return true;
    }

    if (it->second.state != MatchStatusSnapshot::State::kQueued) {
        // 不在队列中（已被配对 / 已超时 / 已取消）都按幂等成功处理。
        return false;
    }

    entries_.erase(it);
    RemoveFromQueueLocked(player_id);
    return true;
}

MatchStatusSnapshot MatchQueue::GetStatus(const std::string& player_id, std::int64_t now_ms) {
    MatchStatusSnapshot snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        SweepLocked(now_ms);
        snapshot = SnapshotLocked(player_id);
    }

    // 惰性重试：上一次房间分配失败时玩家已退回队列。客户端会持续轮询本接口，
    // 因此在这里顺带重试一次配对，玩家不需要重新入队。
    //
    // 返回的是重试**之前**的快照：这一次调用可能把玩家从 queued 变成 matched，
    // 但那是本次调用产生的新事实，下一次轮询才应该看到。假装它已经发生会让调用方
    // 拿到一个与本次请求无关的状态。
    RetryPairingIfNeeded(now_ms);
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

void MatchQueue::SweepLocked(std::int64_t now_ms) {
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
        } else {
            ++it;
        }
    }
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
