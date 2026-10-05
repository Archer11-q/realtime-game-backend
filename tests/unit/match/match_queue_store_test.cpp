/// @file match_queue_store_test.cpp
/// @brief 匹配队列快照的编解码与重启恢复单元测试（TASK-015）。
///
/// 为什么这些用例不碰 Redis：真正容易出错的是**格式与恢复规则**
/// （未知版本、截断字段、计数越界、超时重算、分配中条目的处置），
/// 而这些都可以用纯函数 + 一个假存储精确覆盖。真实 Redis 由
/// scripts/verify-persistence.sh 的「队列恢复」一节端到端验证。

#include "match_queue_store.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "match_queue.hpp"
#include "room_allocator.hpp"

namespace {

using rgbt::match::EnqueueOutcome;
using rgbt::match::MatchQueue;
using rgbt::match::MatchQueueEntry;
using rgbt::match::MatchQueueSnapshot;
using rgbt::match::MatchQueueSnapshotRow;
using rgbt::match::MatchQueueStore;
using rgbt::match::MatchRestoreReport;
using rgbt::match::MatchStatusSnapshot;
using rgbt::match::RoomAllocator;
using rgbt::match::SnapshotEntryKind;

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

/// 可控的房间分配器。
class FakeRoomAllocator : public RoomAllocator {
public:
    bool fail = false;
    /// 分配期间要执行的动作（用于在"分配中"这一瞬间观察队列状态）。
    std::function<void()> on_allocate;

    std::string Allocate(std::string_view match_id, const std::vector<std::string>& /*player_ids*/,
                         std::string_view /*request_id*/) override {
        if (on_allocate) {
            on_allocate();
        }
        if (fail || match_id.empty()) {
            return {};
        }
        return "room-" + std::string(match_id);
    }
};

/// 内存快照存储：记录每次 Save 的内容，并按需返回预设的行。
class FakeMatchQueueStore : public MatchQueueStore {
public:
    bool save_ok = true;
    bool load_ok = true;
    std::vector<MatchQueueSnapshotRow> rows_to_load;
    std::vector<MatchQueueSnapshot> saved;
    int save_calls = 0;
    int load_calls = 0;

    bool Save(const MatchQueueSnapshot& snapshot) override {
        ++save_calls;
        saved.push_back(snapshot);
        return save_ok;
    }

    bool Load(std::vector<MatchQueueSnapshotRow>* out_rows) override {
        ++load_calls;
        if (!load_ok) {
            return false;
        }
        *out_rows = rows_to_load;
        return true;
    }

    bool IsHealthy() override { return save_ok && load_ok; }

    /// 最近一次写入的快照里，某个玩家是否作为「排队中」出现。
    bool LastSnapshotHasQueued(const std::string& player_id) const {
        if (saved.empty()) {
            return false;
        }
        for (const MatchQueueEntry& entry : saved.back().queued) {
            if (entry.player_id == player_id) {
                return true;
            }
        }
        return false;
    }
};

MatchQueueSnapshotRow QueuedRow(const std::string& player_id, std::int64_t queued_at_ms) {
    MatchQueueSnapshotRow row;
    row.entry.kind = SnapshotEntryKind::kQueued;
    row.entry.player_id = player_id;
    row.entry.queued_at_ms = queued_at_ms;
    return row;
}

MatchQueueSnapshotRow MatchedRow(const std::string& match_id, const std::string& room_id,
                                 const std::vector<std::string>& player_ids,
                                 std::int64_t stamp_ms) {
    MatchQueueSnapshotRow row;
    row.entry.kind = SnapshotEntryKind::kMatched;
    row.entry.match_id = match_id;
    row.entry.room_id = room_id;
    row.entry.player_ids = player_ids;
    row.entry.stamp_ms = stamp_ms;
    return row;
}

// ---------------------------------------------------------------------------
// 编解码
// ---------------------------------------------------------------------------

TEST(MatchQueueStoreTest, QueuedEntryRoundTrips) {
    MatchQueueEntry entry;
    entry.kind = SnapshotEntryKind::kQueued;
    entry.player_id = "p-0001";
    entry.queued_at_ms = kT0;

    std::string line;
    ASSERT_TRUE(rgbt::match::EncodeMatchQueueEntry(entry, &line));

    const MatchQueueSnapshotRow decoded = rgbt::match::DecodeMatchQueueEntry(line);
    EXPECT_TRUE(decoded.problem.empty()) << decoded.problem;
    EXPECT_EQ(decoded.entry.kind, SnapshotEntryKind::kQueued);
    EXPECT_EQ(decoded.entry.player_id, "p-0001");
    EXPECT_EQ(decoded.entry.queued_at_ms, kT0);
}

TEST(MatchQueueStoreTest, MatchedEntryRoundTrips) {
    MatchQueueEntry entry;
    entry.kind = SnapshotEntryKind::kMatched;
    entry.match_id = "m-1";
    entry.room_id = "r-1";
    entry.player_ids = {"p-0001", "p-0002"};
    entry.stamp_ms = kT0 + 5;

    std::string line;
    ASSERT_TRUE(rgbt::match::EncodeMatchQueueEntry(entry, &line));

    const MatchQueueSnapshotRow decoded = rgbt::match::DecodeMatchQueueEntry(line);
    EXPECT_TRUE(decoded.problem.empty()) << decoded.problem;
    EXPECT_EQ(decoded.entry.kind, SnapshotEntryKind::kMatched);
    EXPECT_EQ(decoded.entry.match_id, "m-1");
    EXPECT_EQ(decoded.entry.room_id, "r-1");
    EXPECT_EQ(decoded.entry.stamp_ms, kT0 + 5);
    ASSERT_EQ(decoded.entry.player_ids.size(), 2U);
    EXPECT_EQ(decoded.entry.player_ids[0], "p-0001");
    EXPECT_EQ(decoded.entry.player_ids[1], "p-0002");
}

TEST(MatchQueueStoreTest, EncodeRejectsInvalidEntries) {
    std::string line;

    MatchQueueEntry queued;
    queued.kind = SnapshotEntryKind::kQueued;
    queued.player_id = "";
    queued.queued_at_ms = kT0;
    EXPECT_FALSE(rgbt::match::EncodeMatchQueueEntry(queued, &line));

    queued.player_id = "p-0001";
    queued.queued_at_ms = 0;
    EXPECT_FALSE(rgbt::match::EncodeMatchQueueEntry(queued, &line));

    MatchQueueEntry matched;
    matched.kind = SnapshotEntryKind::kMatched;
    matched.match_id = "m-1";
    matched.room_id = "r-1";
    matched.stamp_ms = kT0;
    matched.player_ids = {};
    EXPECT_FALSE(rgbt::match::EncodeMatchQueueEntry(matched, &line));
}

TEST(MatchQueueStoreTest, DecodeRejectsMalformedLines) {
    // 未知版本必须被拒绝：按当前格式强行解析正是"解析出一支乱七八糟的队列"的来源。
    EXPECT_FALSE(rgbt::match::DecodeMatchQueueEntry("9Q3:p-00011700000000000;").problem.empty());
    // 未知条目类型。
    EXPECT_FALSE(rgbt::match::DecodeMatchQueueEntry("1X3:p-0001").problem.empty());
    // 空行。
    EXPECT_FALSE(rgbt::match::DecodeMatchQueueEntry("").problem.empty());
    // player_id 字段截断（声明 10 字节实际只有 6 字节）。
    EXPECT_FALSE(rgbt::match::DecodeMatchQueueEntry("1Q10:p-0001").problem.empty());
    // 整数缺结尾的 ';'。
    EXPECT_FALSE(rgbt::match::DecodeMatchQueueEntry("1Q6:p-00011700000000000").problem.empty());
    // 行尾有多余字节。
    EXPECT_FALSE(
        rgbt::match::DecodeMatchQueueEntry("1Q6:p-00011700000000000;extra").problem.empty());
    // 标识超长（65 字节）。
    const std::string long_id(65, 'x');
    EXPECT_FALSE(
        rgbt::match::DecodeMatchQueueEntry("1Q65:" + long_id + "1700000000000;").problem.empty());
}

TEST(MatchQueueStoreTest, DecodeRejectsOutOfRangePlayerCount) {
    // 计数为 0。
    EXPECT_FALSE(
        rgbt::match::DecodeMatchQueueEntry("1M3:m-13:r-11700000000000;0;").problem.empty());
    // 计数远超上界：必须**先卡上界再循环**，否则损坏的行会让解析器空转。
    EXPECT_FALSE(
        rgbt::match::DecodeMatchQueueEntry("1M3:m-13:r-11700000000000;999999999;").problem.empty());
}

// ---------------------------------------------------------------------------
// 恢复
// ---------------------------------------------------------------------------

class MatchQueueRestoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue_ = std::make_unique<MatchQueue>(
            &allocator_, []() { return std::string("m-fixed"); }, /*match_timeout_ms=*/1000,
            /*result_ttl_ms=*/5000, /*max_queue_size=*/100, &store_);
    }

    FakeRoomAllocator allocator_;
    FakeMatchQueueStore store_;
    std::unique_ptr<MatchQueue> queue_;
};

TEST_F(MatchQueueRestoreTest, RestoreRebuildsFifoOrderByQueuedAt) {
    // 存储里的顺序是反的，恢复必须按 queued_at_ms 重建 FIFO。
    store_.rows_to_load = {QueuedRow("p-0003", kT0 + 300), QueuedRow("p-0001", kT0 + 100),
                           QueuedRow("p-0002", kT0 + 200)};

    const MatchRestoreReport report = queue_->Restore(kT0 + 400);
    EXPECT_FALSE(report.load_failed);
    EXPECT_EQ(report.queued, 3U);
    EXPECT_EQ(queue_->QueueSize(), 3U);

    // 再入队一个人会触发配对：应当取走队首的两人（p-0001 与 p-0002），
    // 这就验证了恢复后的顺序而不只是"人数对得上"。
    EXPECT_EQ(queue_->Enqueue("p-0004", "req-4", kT0 + 400), EnqueueOutcome::kQueued);
    const MatchStatusSnapshot first = queue_->GetStatus("p-0001", kT0 + 400);
    EXPECT_EQ(first.state, MatchStatusSnapshot::State::kMatched);
    ASSERT_EQ(first.player_ids.size(), 2U);
    EXPECT_EQ(first.player_ids[0], "p-0001");
    EXPECT_EQ(first.player_ids[1], "p-0002");
}

TEST_F(MatchQueueRestoreTest, RestoreRecomputesTimeoutUsingDowntime) {
    store_.rows_to_load = {QueuedRow("p-0001", kT0), QueuedRow("p-0002", kT0 + 900)};

    // 停机 1500 ms 后启动：p-0001 的等待已经超过 1000 ms 的上限，p-0002 还没有。
    const MatchRestoreReport report = queue_->Restore(kT0 + 1500);
    EXPECT_EQ(report.timed_out, 1U);
    EXPECT_EQ(report.queued, 1U);
    EXPECT_EQ(queue_->QueueSize(), 1U);
    // 已超时的那条**不恢复**（而不是装进内存标成 timeout）：玩家该做的是重新入队，
    // 而"队列里没有他"正是这个语义最直接的表达。
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 1500).state, MatchStatusSnapshot::State::kIdle);
    EXPECT_EQ(queue_->GetStatus("p-0002", kT0 + 1500).state, MatchStatusSnapshot::State::kQueued);
}

TEST_F(MatchQueueRestoreTest, RestoreRebuildsMatchedResultsForEveryPlayer) {
    store_.rows_to_load = {
        MatchedRow("m-7", "r-7", {"p-0001", "p-0002"}, kT0 + 100),
    };

    const MatchRestoreReport report = queue_->Restore(kT0 + 200);
    EXPECT_EQ(report.matched, 2U);

    // 两个玩家都必须看到这一局，否则另一个玩家会被当成"从没排过队"。
    for (const char* player_id : {"p-0001", "p-0002"}) {
        const MatchStatusSnapshot status = queue_->GetStatus(player_id, kT0 + 200);
        EXPECT_EQ(status.state, MatchStatusSnapshot::State::kMatched) << player_id;
        EXPECT_EQ(status.match_id, "m-7") << player_id;
        EXPECT_EQ(status.room_id, "r-7") << player_id;
    }
}

TEST_F(MatchQueueRestoreTest, RestoreExpiresMatchedResultsPastTtl) {
    store_.rows_to_load = {MatchedRow("m-7", "r-7", {"p-0001", "p-0002"}, kT0)};

    // 结果保留 5000 ms，停机 6000 ms：结果已经过期，**不恢复**，玩家回到空闲。
    const MatchRestoreReport report = queue_->Restore(kT0 + 6000);
    EXPECT_EQ(report.matched, 0U);
    EXPECT_EQ(report.expired_results, 2U);
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 6000).state, MatchStatusSnapshot::State::kIdle);
}

TEST_F(MatchQueueRestoreTest, RestoreSkipsCorruptRowsAndKeepsTheRest) {
    MatchQueueSnapshotRow corrupt = QueuedRow("p-0009", kT0);
    corrupt.problem = "未知的快照格式版本：9";
    store_.rows_to_load = {corrupt, QueuedRow("p-0001", kT0 + 100)};

    const MatchRestoreReport report = queue_->Restore(kT0 + 200);
    EXPECT_EQ(report.scanned, 2U);
    EXPECT_EQ(report.dropped, 1U);
    EXPECT_EQ(report.queued, 1U);
    EXPECT_EQ(queue_->GetStatus("p-0001", kT0 + 200).state, MatchStatusSnapshot::State::kQueued);
    // 损坏的那条**不静默丢弃**：它没有变成队列条目，也没有被当成正常记录。
    EXPECT_EQ(queue_->GetStatus("p-0009", kT0 + 200).state, MatchStatusSnapshot::State::kIdle);
}

TEST_F(MatchQueueRestoreTest, RestoreOnUnavailableStoreRestoresNothing) {
    store_.load_ok = false;
    store_.rows_to_load = {QueuedRow("p-0001", kT0)};

    const MatchRestoreReport report = queue_->Restore(kT0 + 100);
    EXPECT_TRUE(report.load_failed);
    // 空手启动：**不恢复任何条目**，也不假装恢复了一个空队列。
    EXPECT_EQ(report.queued, 0U);
    EXPECT_EQ(queue_->QueueSize(), 0U);
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------

TEST_F(MatchQueueRestoreTest, EnqueueWritesSnapshotOnNextTick) {
    // TASK-028：入队只在请求路径上打"待写"标记，**一个字节都不写 Redis**。
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    EXPECT_EQ(store_.save_calls, 0) << "请求路径不该写快照";
    EXPECT_TRUE(queue_->SnapshotPending());

    queue_->Tick(kT0 + 1000);  // 越过合并窗口
    EXPECT_EQ(store_.save_calls, 1);
    EXPECT_TRUE(store_.LastSnapshotHasQueued("p-0001"));
}

TEST_F(MatchQueueRestoreTest, PollingWithoutStateChangeDoesNotWriteSnapshots) {
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    const int after_enqueue = store_.save_calls;

    // 轮询是高频路径：只在清算或配对真的改变状态时才写快照，
    // 否则每个客户端的每次轮询都会变成一次 Redis 写。
    for (int i = 0; i < 5; ++i) {
        // 只为驱动惰性清算路径，返回值与本用例的断言无关。
        (void)queue_->GetStatus("p-0001", kT0 + 10 * (i + 1));
    }
    EXPECT_EQ(store_.save_calls, after_enqueue);
}

TEST_F(MatchQueueRestoreTest, PairingWritesMatchedResultIntoSnapshot) {
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    ASSERT_EQ(queue_->Enqueue("p-0002", "req-2", kT0), EnqueueOutcome::kQueued);
    queue_->Tick(kT0 + 1000);  // TASK-028：写入由 Tick 合并执行

    ASSERT_FALSE(store_.saved.empty());
    const MatchQueueSnapshot& snapshot = store_.saved.back();
    ASSERT_EQ(snapshot.matched.size(), 1U);
    EXPECT_EQ(snapshot.matched[0].kind, SnapshotEntryKind::kMatched);
    EXPECT_EQ(snapshot.matched[0].room_id, "room-m-fixed");
    // 一局只写一条记录，而不是每个玩家各写一条。
    ASSERT_EQ(snapshot.matched[0].player_ids.size(), 2U);
    EXPECT_TRUE(snapshot.queued.empty());
}

TEST_F(MatchQueueRestoreTest, CancelWritesSnapshot) {
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    queue_->Tick(kT0);
    const int before_cancel = store_.save_calls;

    // 注意时间：队列的排队超时是 1000 ms，取消必须发生在超时之前（否则宿清会先把它
    // 淘汰掉，Cancel 变成幂等的"不在队列中"）。刷写的时间可以另算。
    EXPECT_TRUE(queue_->Cancel("p-0001", kT0 + 10));
    EXPECT_EQ(store_.save_calls, before_cancel) << "取消也只在请求路径打标记";
    queue_->Tick(kT0 + 1000);
    EXPECT_GT(store_.save_calls, before_cancel);
    EXPECT_FALSE(store_.LastSnapshotHasQueued("p-0001"));
}

TEST_F(MatchQueueRestoreTest, SaveFailureDoesNotBlockEnqueueOrPairing) {
    // 这是本任务的核心失败策略：Redis 不可用时降级为纯内存，匹配照常工作。
    store_.save_ok = false;

    EXPECT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    EXPECT_EQ(queue_->Enqueue("p-0002", "req-2", kT0), EnqueueOutcome::kQueued);

    const MatchStatusSnapshot status = queue_->GetStatus("p-0001", kT0);
    EXPECT_EQ(status.state, MatchStatusSnapshot::State::kMatched);
    EXPECT_FALSE(status.room_id.empty());

    queue_->Tick(kT0 + 1000);
    EXPECT_GT(store_.save_calls, 0);  // 确实尝试过写，只是失败了
    EXPECT_EQ(queue_->SnapshotWriteCount(), 0U);
    EXPECT_EQ(queue_->SnapshotFailureCount(), 1U) << "失败必须被计数（降级可见）";
    EXPECT_FALSE(queue_->SnapshotPending()) << "失败后不重试：策略是丢弃";
}

TEST_F(MatchQueueRestoreTest, AllocatingEntriesAreSnapshottedAsQueued) {
    // 「分配中」的条目必须以**排队中**的形式落进快照，重启后才会退回队列（决策 4）。
    // 同一用例顺带验证另一半规则：**已登记取消意图的条目不写**——客户端的取消
    // 已经答复成功，重启后让它重新出现在队列里会与那个答复矛盾。
    //
    // TASK-028 之后抓法要改一处前提：写入不再由"取消"顺带触发，而是由主线程的
    // Tick 驱动——而 Tick **可能正好落在分配过程中**。因此用例在分配回调里显式驱动
    // 一次 Tick，检验那个时刻的快照把"分配中"写成了排队中（决策 4）。
    ASSERT_EQ(queue_->Enqueue("p-0003", "req-3", kT0), EnqueueOutcome::kQueued);

    std::vector<MatchQueueSnapshot> during_allocate;
    allocator_.on_allocate = [this, &during_allocate]() {
        const std::size_t before = store_.saved.size();
        queue_->Cancel("p-0003", kT0 + 1);
        queue_->Tick(kT0 + 2);  // 模拟刷写正好落在分配过程中
        for (std::size_t i = before; i < store_.saved.size(); ++i) {
            during_allocate.push_back(store_.saved[i]);
        }
    };

    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);

    ASSERT_FALSE(during_allocate.empty()) << "分配期间没有产生快照，本用例的前提不成立";
    const MatchQueueSnapshot& snapshot = during_allocate.back();
    ASSERT_EQ(snapshot.queued.size(), 1U);
    EXPECT_EQ(snapshot.queued[0].player_id, "p-0001");
    EXPECT_EQ(snapshot.queued[0].kind, SnapshotEntryKind::kQueued);
    EXPECT_TRUE(snapshot.matched.empty());
}

// ---------------------------------------------------------------------------
// TASK-028：请求路径只打标记，写入由 Tick 合并执行
// ---------------------------------------------------------------------------

TEST_F(MatchQueueRestoreTest, MergesChangesInsideTheWindowIntoOneWrite) {
    // 合并窗口内的多次变化只落地**一份最新**快照——这正是"每秒 276 次全量写"降下来的
    // 原因，也是入队 p95 从 840 ms 掉下来的原因。
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    queue_->Tick(kT0);  // 第一次没有"上次写入"可比，因此立刻写
    ASSERT_EQ(store_.save_calls, 1);

    // 窗口内继续变化：p-0002 与 p-0003 会配成一对。
    ASSERT_EQ(queue_->Enqueue("p-0002", "req-2", kT0 + 10), EnqueueOutcome::kQueued);
    ASSERT_EQ(queue_->Enqueue("p-0003", "req-3", kT0 + 20), EnqueueOutcome::kQueued);

    queue_->Tick(kT0 + 50);  // 默认窗口 100 ms，此刻仍在窗口内
    EXPECT_EQ(store_.save_calls, 1) << "窗口内的变化不该各自触发一次写入";
    EXPECT_GT(queue_->SnapshotMergedCount(), 0U) << "被合并掉的变化数必须可见";
    EXPECT_TRUE(queue_->SnapshotPending());

    queue_->Tick(kT0 + 200);  // 越过窗口
    EXPECT_EQ(store_.save_calls, 2);
    EXPECT_FALSE(queue_->SnapshotPending());
}

TEST_F(MatchQueueRestoreTest, ZeroMergeIntervalWritesOnEveryTick) {
    // 关掉合并时要退回"每次 Tick 都写"的行为，便于对照与排障。
    queue_->SetSnapshotMergeIntervalMs(0);

    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    queue_->Tick(kT0);
    EXPECT_EQ(store_.save_calls, 1);

    ASSERT_EQ(queue_->Enqueue("p-0002", "req-2", kT0), EnqueueOutcome::kQueued);
    queue_->Tick(kT0);  // 同一毫秒也要写
    EXPECT_EQ(store_.save_calls, 2);
}

TEST_F(MatchQueueRestoreTest, FinalFlushWritesLastChangeBeforeShutdown) {
    // 关机契约：**排空返回之前，最后一次变化必须已经落盘**。异步写入不能把
    // "丢最后一次变化"从"进程被 kill -9"扩大到"正常 SIGTERM 也会丢"。
    queue_->SetSnapshotMergeIntervalMs(10 * 1000);  // 窗口拉长到 10 秒

    // 先制造一次写入（第一次 Tick 总会写：还没有"上次写入"可比）。
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    queue_->Tick(kT0);
    ASSERT_EQ(store_.save_calls, 1);

    // 窗口内再变化一次：这次 Tick **不该**写。
    ASSERT_EQ(queue_->Enqueue("p-0002", "req-2", kT0 + 1), EnqueueOutcome::kQueued);
    queue_->Tick(kT0 + 2);
    EXPECT_EQ(store_.save_calls, 1) << "窗口没到，Tick 不该写";
    EXPECT_TRUE(queue_->SnapshotPending());

    // 关机前的最终刷写忽略窗口：**排空返回之前最后一次变化必须落盘**。
    EXPECT_TRUE(queue_->FlushSnapshotNow(kT0 + 3));
    EXPECT_EQ(store_.save_calls, 2);
    ASSERT_FALSE(store_.saved.empty());
    EXPECT_EQ(store_.saved.back().matched.size(), 1U) << "最后一次变化（配对）必须落地";

    EXPECT_FALSE(queue_->FlushSnapshotNow(kT0 + 4)) << "没有变化就不该写";
    EXPECT_EQ(store_.save_calls, 2);
}

TEST_F(MatchQueueRestoreTest, SnapshotLagReportsHowLongAChangeHasWaited) {
    // 待写滞后是"有变化一直没落地"的唯一直接证据：健康时应小于合并窗口。
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    EXPECT_TRUE(queue_->SnapshotPending());
    EXPECT_EQ(queue_->SnapshotLagMs(kT0 + 30), 30U);

    queue_->Tick(kT0 + 1000);
    EXPECT_FALSE(queue_->SnapshotPending());
    EXPECT_EQ(queue_->SnapshotLagMs(kT0 + 1030), 0U);
}

TEST_F(MatchQueueRestoreTest, AllocationWorkerPairsWithoutBlockingCallers) {
    // TASK-035：worker 启动后，请求路径只把待分配组交给 worker；分配与提交发生在
    // worker 线程上，请求线程不再阻塞在 Room 调用里。
    queue_->StartAllocationWorker();

    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    ASSERT_EQ(queue_->Enqueue("p-0002", "req-2", kT0), EnqueueOutcome::kQueued);
    queue_->WaitForIdleAllocations();  // 等 worker 提交（不用固定 sleep）

    const MatchStatusSnapshot status = queue_->GetStatus("p-0001", kT0 + 10);
    EXPECT_EQ(status.state, MatchStatusSnapshot::State::kMatched);
    ASSERT_EQ(status.player_ids.size(), 2U);
    EXPECT_FALSE(status.room_id.empty());

    queue_->StopAllocationWorker();
}

TEST_F(MatchQueueRestoreTest, AllocationWorkerStopsCleanlyAndKeepsSnapshotContract) {
    // 停机契约：worker 停掉之后，最后一次变化仍要能通过最终刷写落盘。
    queue_->StartAllocationWorker();
    ASSERT_EQ(queue_->Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    queue_->StopAllocationWorker();

    EXPECT_TRUE(queue_->FlushSnapshotNow(kT0 + 1));
    EXPECT_TRUE(store_.LastSnapshotHasQueued("p-0001"));
}

TEST_F(MatchQueueRestoreTest, NoStoreMeansNoRestoreAndNoWrite) {
    MatchQueue plain(&allocator_, []() { return std::string("m-fixed"); }, 1000, 5000);
    const MatchRestoreReport report = plain.Restore(kT0);
    EXPECT_FALSE(report.load_failed);
    EXPECT_EQ(report.scanned, 0U);
    EXPECT_EQ(plain.Enqueue("p-0001", "req-1", kT0), EnqueueOutcome::kQueued);
    EXPECT_EQ(plain.QueueSize(), 1U);
}

}  // namespace
