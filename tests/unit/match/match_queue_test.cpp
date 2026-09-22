/// @file match_queue_test.cpp
/// @brief 匹配队列与配对逻辑的单元测试。
///
/// 这些用例不启动 brpc 服务器、不访问 Redis/MySQL：队列逻辑放在不依赖框架的
/// MatchQueue 里，正是为了让配对、取消、超时这些规则能被精确覆盖。
///
/// 时间由用例注入（每个调用都传 now_ms），因此超时与过期不需要 sleep，
/// 也不会出现「偶发失败」。

#include "match_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "room_allocator.hpp"

namespace {

using rgbt::match::DerivedRoomAllocator;
using rgbt::match::EnqueueOutcome;
using rgbt::match::MatchQueue;
using rgbt::match::MatchStatusSnapshot;
using rgbt::match::RoomAllocator;

/// 可控的房间分配器，用于验证分配失败时的行为。
class FakeRoomAllocator : public RoomAllocator {
public:
    bool fail = false;
    int allocate_calls = 0;

    std::string Allocate(std::string_view match_id) override {
        ++allocate_calls;
        if (fail || match_id.empty()) {
            return {};
        }
        return "room-" + std::string(match_id);
    }
};

/// 时间基准。用例都在它之上加减，避免依赖真实时钟。
constexpr std::int64_t kT0 = 1'700'000'000'000LL;

/// 测试夹具：固定超时与结果保留时长，并让 match_id 可预测。
class MatchQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue_ = std::make_unique<MatchQueue>(
            &allocator_, []() { return std::string("m-fixed"); }, 1000, 5000);
    }

    FakeRoomAllocator allocator_;
    std::unique_ptr<MatchQueue> queue_;
};

// ---------------------------------------------------------------------------
// 入队与配对
// ---------------------------------------------------------------------------

TEST_F(MatchQueueTest, SinglePlayerStaysQueued) {
    EXPECT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);

    const MatchStatusSnapshot status = queue_->GetStatus("p-0001", kT0);
    EXPECT_EQ(status.state, MatchStatusSnapshot::State::kQueued);
    EXPECT_EQ(status.request_id, "req-1");
    EXPECT_EQ(status.queued_at_ms, kT0);
    EXPECT_EQ(status.queue_size, 1U);
    EXPECT_TRUE(status.match_id.empty());
    EXPECT_EQ(queue_->QueueSize(), 1U);
}

TEST_F(MatchQueueTest, SecondPlayerFormsAMatch) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    EXPECT_EQ(queue_->Enqueue("p-0002", "req-2", kT0), EnqueueOutcome::kQueued);

    for (const char* player_id : {"p-0001", "p-0002"}) {
        const MatchStatusSnapshot status = queue_->GetStatus(player_id, kT0);
        EXPECT_EQ(status.state, MatchStatusSnapshot::State::kMatched) << player_id;
        EXPECT_EQ(status.match_id, "m-fixed") << player_id;
        // 同一局的双方必须拿到同一个 room_id，否则进不了同一个房间。
        EXPECT_EQ(status.room_id, "room-m-fixed") << player_id;
        ASSERT_EQ(status.player_ids.size(), 2U) << player_id;
        EXPECT_EQ(status.player_ids[0], "p-0001") << player_id;
        EXPECT_EQ(status.player_ids[1], "p-0002") << player_id;
    }
    // 配对成功后双方都离开队列。
    EXPECT_EQ(queue_->QueueSize(), 0U);
}

TEST_F(MatchQueueTest, FifoOrderDecidesWhoIsPairedFirst) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    queue_->Enqueue("p-0002", "req-2", kT0 + 1);
    queue_->Enqueue("p-0003", "req-3", kT0 + 2);

    // 前两个入队的人先配对，第三人继续等待——这是「先来先服务」的直接验证。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 2).state, MatchStatusSnapshot::State::kMatched);
    EXPECT_EQ(queue_->GetStatus("p-0002", kT0 + 2).state, MatchStatusSnapshot::State::kMatched);
    EXPECT_EQ(queue_->GetStatus("p-0003", kT0 + 2).state, MatchStatusSnapshot::State::kQueued);
    EXPECT_EQ(queue_->QueueSize(), 1U);
}

TEST_F(MatchQueueTest, CompletedMatchDoesNotAbsorbLaterPlayers) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    queue_->Enqueue("p-0002", "req-2", kT0);

    // 第一局已配满，第三个玩家不能挤进已有的局。
    queue_->Enqueue("p-0003", "req-3", kT0 + 10);

    const MatchStatusSnapshot first = queue_->GetStatus("p-0001", kT0 + 10);
    ASSERT_EQ(first.player_ids.size(), 2U);
    EXPECT_EQ(first.player_ids[1], "p-0002");

    const MatchStatusSnapshot third = queue_->GetStatus("p-0003", kT0 + 10);
    EXPECT_EQ(third.state, MatchStatusSnapshot::State::kQueued);
}

TEST_F(MatchQueueTest, FourPlayersFormTwoIndependentMatches) {
    // match_id 固定为 m-fixed，这里用一个递增值的生成器区分两局。
    int counter = 0;
    MatchQueue queue(
        &allocator_, [&counter]() { return "m-" + std::to_string(++counter); }, 1000, 5000);

    queue.Enqueue("p-0001", "req-1", kT0);
    queue.Enqueue("p-0002", "req-2", kT0);
    queue.Enqueue("p-0003", "req-3", kT0);
    queue.Enqueue("p-0004", "req-4", kT0);

    const MatchStatusSnapshot first = queue.GetStatus("p-0001", kT0);
    const MatchStatusSnapshot third = queue.GetStatus("p-0003", kT0);
    EXPECT_EQ(first.match_id, "m-1");
    EXPECT_EQ(third.match_id, "m-2");
    EXPECT_NE(first.match_id, third.match_id);
    EXPECT_NE(first.room_id, third.room_id);
    EXPECT_EQ(queue.QueueSize(), 0U);
}

TEST_F(MatchQueueTest, PlayerNeverAppearsInTwoMatches) {
    int counter = 0;
    MatchQueue queue(
        &allocator_, [&counter]() { return "m-" + std::to_string(++counter); }, 1000, 5000);

    for (int i = 1; i <= 4; ++i) {
        queue.Enqueue("p-000" + std::to_string(i), "req-" + std::to_string(i), kT0);
    }

    // 架构要求「同一玩家不重复进入多个有效队列或匹配结果」。
    // 用「每个玩家恰好有一条记录，且每局恰好两人」来验证这个不变量。
    int matched = 0;
    for (int i = 1; i <= 4; ++i) {
        const MatchStatusSnapshot status = queue.GetStatus("p-000" + std::to_string(i), kT0);
        ASSERT_EQ(status.state, MatchStatusSnapshot::State::kMatched);
        ASSERT_EQ(status.player_ids.size(), 2U);
        // 自己只应在自己那一局里出现一次。
        int occurrences = 0;
        for (const std::string& id : status.player_ids) {
            if (id == "p-000" + std::to_string(i)) {
                ++occurrences;
            }
        }
        EXPECT_EQ(occurrences, 1);
        ++matched;
    }
    EXPECT_EQ(matched, 4);
}

// ---------------------------------------------------------------------------
// 重复入队（幂等）
// ---------------------------------------------------------------------------

TEST_F(MatchQueueTest, RepeatEnqueueWhileQueuedIsIdempotent) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    EXPECT_EQ(queue_->Enqueue("p-0001", "req-1", kT0 + 1), EnqueueOutcome::kAlreadyQueued);
    EXPECT_EQ(queue_->Enqueue("p-0001", "req-other", kT0 + 2), EnqueueOutcome::kAlreadyQueued);

    // 队列里仍然只有一条记录，且 request_id 保持第一次的值（Gettstatus 反映原始请求）。
    EXPECT_EQ(queue_->QueueSize(), 1U);
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 2).request_id, "req-1");
}

TEST_F(MatchQueueTest, RepeatEnqueueDoesNotOverwriteMatchedResult) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    queue_->Enqueue("p-0002", "req-2", kT0);

    // 玩家已配对但还没领取结果，此时再次入队不能把结果冲掉，
    // 否则「匹配成功」与「客户端下一次轮询」之间的竞态会让玩家丢掉这一局。
    EXPECT_EQ(queue_->Enqueue("p-0001", "req-3", kT0 + 1), EnqueueOutcome::kAlreadyQueued);
    const MatchStatusSnapshot status = queue_->GetStatus("p-0001", kT0 + 1);
    EXPECT_EQ(status.state, MatchStatusSnapshot::State::kMatched);
    EXPECT_EQ(status.match_id, "m-fixed");
}

// ---------------------------------------------------------------------------
// 取消
// ---------------------------------------------------------------------------

TEST_F(MatchQueueTest, CancelRemovesPlayerFromQueue) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    EXPECT_TRUE(queue_->Cancel("p-0001", kT0 + 1));
    EXPECT_EQ(queue_->QueueSize(), 0U);
    // 取消后回到「无记录」，即空闲状态。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 1).state, MatchStatusSnapshot::State::kIdle);
}

TEST_F(MatchQueueTest, CancelledPlayerIsNotPairedLater) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    queue_->Cancel("p-0001", kT0 + 1);
    queue_->Enqueue("p-0002", "req-2", kT0 + 2);

    // 第二个人入队时队列里已经没有 p-0001，因此他不应该被配对。
    EXPECT_EQ(queue_->GetStatus("p-0002", kT0 + 2).state, MatchStatusSnapshot::State::kQueued);
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 2).state, MatchStatusSnapshot::State::kIdle);
}

TEST_F(MatchQueueTest, CancelIsIdempotentWhenNotQueued) {
    // 从未入队。
    EXPECT_FALSE(queue_->Cancel("p-0001", kT0));
    // 已取消。
    queue_->Enqueue("p-0002", "req-2", kT0);
    EXPECT_TRUE(queue_->Cancel("p-0002", kT0 + 1));
    EXPECT_FALSE(queue_->Cancel("p-0002", kT0 + 2));
}

TEST_F(MatchQueueTest, CancelAfterMatchedKeepsTheResult) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    queue_->Enqueue("p-0002", "req-2", kT0);

    // 已配对成功后取消：不能把别人的匹配结果一起删掉。
    EXPECT_FALSE(queue_->Cancel("p-0001", kT0 + 1));
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 1).state, MatchStatusSnapshot::State::kMatched);
    EXPECT_EQ(queue_->GetStatus("p-0002", kT0 + 1).state, MatchStatusSnapshot::State::kMatched);
}

// ---------------------------------------------------------------------------
// 超时与过期
// ---------------------------------------------------------------------------

TEST_F(MatchQueueTest, PlayerIsEvictedAfterMatchTimeout) {
    queue_->Enqueue("p-0001", "req-1", kT0);

    // 超时前仍然是排队中。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 999).state, MatchStatusSnapshot::State::kQueued);
    // 到达超时阈值后被淘汰，并进入 timeout 状态（与 idle 区分）。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 1000).state, MatchStatusSnapshot::State::kTimeout);
    EXPECT_EQ(queue_->QueueSize(), 0U);
}

TEST_F(MatchQueueTest, TimedOutPlayerCanEnqueueAgain) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    // 查询一次即可触发惰性淘汰（GetStatus 有 [[nodiscard]]，因此显式判断结果）。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 1000).state, MatchStatusSnapshot::State::kTimeout);

    EXPECT_EQ(queue_->Enqueue("p-0001", "req-2", kT0 + 1001), EnqueueOutcome::kQueued);
    const MatchStatusSnapshot status = queue_->GetStatus("p-0001", kT0 + 1001);
    EXPECT_EQ(status.state, MatchStatusSnapshot::State::kQueued);
    EXPECT_EQ(status.request_id, "req-2");
}

TEST_F(MatchQueueTest, TimedOutPlayerDoesNotBlockOthers) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    // p-0001 在排队期间超时。
    queue_->Enqueue("p-0002", "req-2", kT0 + 2000);

    // p-0002 不应该和一个已超时的人配成局；它应该继续排队。
    EXPECT_EQ(queue_->GetStatus("p-0002", kT0 + 2000).state, MatchStatusSnapshot::State::kQueued);
    EXPECT_EQ(queue_->QueueSize(), 1U);
}

TEST_F(MatchQueueTest, MatchedResultExpiresAfterTtl) {
    queue_->Enqueue("p-0001", "req-1", kT0);
    queue_->Enqueue("p-0002", "req-2", kT0);

    // 结果保留 5000ms。到期前可领取。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 4999).state, MatchStatusSnapshot::State::kMatched);
    // 到期后清理，玩家回到空闲。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 5000).state, MatchStatusSnapshot::State::kIdle);
}

// ---------------------------------------------------------------------------
// 上限与输入校验
// ---------------------------------------------------------------------------

TEST_F(MatchQueueTest, QueueFullIsRejected) {
    // 上限 1 且第二个人会立刻配对，因此用一个只有 1 人的场景制造满队列。
    MatchQueue queue(&allocator_, []() { return std::string("m-fixed"); }, 10'000, 10'000, 1);
    EXPECT_EQ(queue.Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    EXPECT_EQ(queue.Enqueue("p-0002", "req-2", kT0), EnqueueOutcome::kQueueFull);
}

TEST_F(MatchQueueTest, EmptyIdentifiersAreRejected) {
    EXPECT_EQ(queue_->Enqueue("", "req-1", kT0), EnqueueOutcome::kInvalidArgument);
    EXPECT_EQ(queue_->Enqueue("p-0001", "", kT0), EnqueueOutcome::kInvalidArgument);
    EXPECT_EQ(queue_->Enqueue(std::string(65, 'p'), "req-1", kT0),
              EnqueueOutcome::kInvalidArgument);
    EXPECT_EQ(queue_->Enqueue("p-0001", std::string(65, 'r'), kT0),
              EnqueueOutcome::kInvalidArgument);
    EXPECT_EQ(queue_->QueueSize(), 0U);
}

TEST_F(MatchQueueTest, UnknownPlayerIsIdle) {
    const MatchStatusSnapshot status = queue_->GetStatus("nobody", kT0);
    EXPECT_EQ(status.state, MatchStatusSnapshot::State::kIdle);
    EXPECT_EQ(status.queue_size, 0U);
    EXPECT_TRUE(status.match_id.empty());
}

// ---------------------------------------------------------------------------
// 房间分配失败
// ---------------------------------------------------------------------------

TEST_F(MatchQueueTest, AllocatorFailureMarksPlayersAsTimedOut) {
    allocator_.fail = true;
    queue_->Enqueue("p-0001", "req-1", kT0);
    queue_->Enqueue("p-0002", "req-2", kT0);

    // 分配不出房间号时不能让玩家无限期留在队列里：标记为超时，
    // 客户端据此提示「重新匹配」。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0).state, MatchStatusSnapshot::State::kTimeout);
    EXPECT_EQ(queue_->GetStatus("p-0002", kT0).state, MatchStatusSnapshot::State::kTimeout);
    EXPECT_EQ(queue_->QueueSize(), 0U);
}

// ---------------------------------------------------------------------------
// 房间分配器本身
// ---------------------------------------------------------------------------

TEST(DerivedRoomAllocatorTest, SameMatchIdYieldsSameRoomId) {
    DerivedRoomAllocator allocator;
    const std::string first = allocator.Allocate("m-abc");
    const std::string second = allocator.Allocate("m-abc");

    // 幂等：同一个 match_id 必须得到同一个 room_id，否则同局双方会进入不同房间。
    EXPECT_EQ(first, second);
    EXPECT_EQ(first, "room-m-abc");
}

TEST(DerivedRoomAllocatorTest, DifferentMatchIdsYieldDifferentRoomIds) {
    DerivedRoomAllocator allocator;
    EXPECT_NE(allocator.Allocate("m-abc"), allocator.Allocate("m-def"));
}

TEST(DerivedRoomAllocatorTest, EmptyMatchIdIsRejected) {
    DerivedRoomAllocator allocator;
    EXPECT_TRUE(allocator.Allocate("").empty());
}

}  // namespace
