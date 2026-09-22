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

    const std::lock_guard<std::mutex> lock(mutex_);
    SweepLocked(now_ms);

    const auto existing = entries_.find(player_id);
    if (existing != entries_.end()) {
        // 排队中或已有未领取的结果：一律按幂等处理，不新建条目。
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

    // 入队后立刻尝试配对，让第二个入队的玩家在同一次调用内就拿到结果。
    TryPairLocked(now_ms);
    return EnqueueOutcome::kQueued;
}

bool MatchQueue::Cancel(const std::string& player_id, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    SweepLocked(now_ms);

    const auto it = entries_.find(player_id);
    if (it == entries_.end() || it->second.state != MatchStatusSnapshot::State::kQueued) {
        // 不在队列中（从未入队 / 已被配对 / 已超时 / 已取消）都按幂等成功处理，
        // 由调用方决定返回 200。这与登出「重复调用不报错」保持一致。
        return false;
    }

    entries_.erase(it);
    RemoveFromQueueLocked(player_id);
    return true;
}

MatchStatusSnapshot MatchQueue::GetStatus(const std::string& player_id, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    SweepLocked(now_ms);
    return SnapshotLocked(player_id);
}

std::size_t MatchQueue::QueueSize() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void MatchQueue::SweepLocked(std::int64_t now_ms) {
    // 1. 淘汰排队超时的玩家。先改状态再从队列里移除，保证两者一致。
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

void MatchQueue::TryPairLocked(std::int64_t now_ms) {
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
            return;
        }

        const std::string match_id = match_id_factory_();
        const std::string room_id = allocator_ != nullptr ? allocator_->Allocate(match_id) : "";

        if (match_id.empty() || room_id.empty()) {
            // 分配失败：把两人标记为超时，让客户端知道需要重新匹配，
            // 而不是让他们无限期留在队列里。
            for (const std::string& player_id : group) {
                auto it = entries_.find(player_id);
                if (it != entries_.end()) {
                    it->second.state = MatchStatusSnapshot::State::kTimeout;
                    it->second.stamp_ms = now_ms;
                }
            }
            continue;
        }

        for (const std::string& player_id : group) {
            auto it = entries_.find(player_id);
            if (it == entries_.end()) {
                continue;
            }
            it->second.state = MatchStatusSnapshot::State::kMatched;
            it->second.match_id = match_id;
            it->second.room_id = room_id;
            it->second.player_ids = group;
            it->second.stamp_ms = now_ms;
        }
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
