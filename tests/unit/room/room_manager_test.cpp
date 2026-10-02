/// @file room_manager_test.cpp
/// @brief RoomManager 的单元测试：创建幂等、状态路由、结果落库与回收。
///
/// 结果存储用假实现注入，因此「MySQL 不可用」「结果尚未落库」这两条最难在集成
/// 环境稳定复现的路径，在这里可以被精确控制。

#include "room_manager.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "match_result_writer.hpp"
#include "room_snapshot_writer.hpp"
#include "room_types.hpp"

namespace {

using rgbt::room::CreateOutcome;
using rgbt::room::JoinOutcome;
using rgbt::room::kFinishedRetentionMs;
using rgbt::room::kFrameIntervalMs;
using rgbt::room::kInitialHp;
using rgbt::room::kMaxFrames;
using rgbt::room::kSnapshotIntervalMs;
using rgbt::room::kWaitingTimeoutMs;
using rgbt::room::MatchResultRecord;
using rgbt::room::MatchResultWriter;
using rgbt::room::ReadStatus;
using rgbt::room::ResultOutcome;
using rgbt::room::RoomManager;
using rgbt::room::RoomPhase;
using rgbt::room::RoomSnapshot;
using rgbt::room::RoomSnapshotRecord;
using rgbt::room::RoomSnapshotWriter;
using rgbt::room::SnapshotWriteStatus;
using rgbt::room::SubmitOutcome;
using rgbt::room::WriteStatus;

/// 时间基准。用例都在它之上加减，避免依赖真实时钟。
constexpr std::int64_t kT0 = 1'700'000'000'000LL;

/// 内存结果存储。
///
/// 以 match_id 为键，因此可以验证「重复写入不产生第二行」这一幂等语义。
class FakeResultWriter : public MatchResultWriter {
public:
    /// 下一次（以及之后所有）写入的返回值。
    WriteStatus next_write = WriteStatus::kOk;
    /// 下一次（以及之后所有）读取的返回值。
    ReadStatus next_read = ReadStatus::kOk;

    std::map<std::string, MatchResultRecord> rows;
    int write_calls = 0;
    int read_calls = 0;

    WriteStatus Write(const MatchResultRecord& record) override {
        ++write_calls;
        if (next_write != WriteStatus::kOk) {
            return next_write;
        }
        // 已存在则不覆盖：与 MySQL 的 ON DUPLICATE KEY UPDATE match_id = match_id 同义。
        rows.emplace(record.match_id, record);
        return WriteStatus::kOk;
    }

    ReadStatus Read(const std::string& match_id, MatchResultRecord* out_record) override {
        ++read_calls;
        if (next_read != ReadStatus::kOk) {
            return next_read;
        }
        const auto it = rows.find(match_id);
        if (it == rows.end()) {
            return ReadStatus::kNotFound;
        }
        if (out_record != nullptr) {
            *out_record = it->second;
        }
        return ReadStatus::kOk;
    }

    bool IsHealthy() override { return true; }
};

/// 到达最大帧数、按 HP 判定胜负的时刻。
constexpr std::int64_t kTimeoutAtMs = kT0 + kMaxFrames * kFrameIntervalMs;

/// 从 from_ms 按帧推进到 target_ms。
///
/// **必须分多次调用**：单次 Tick 最多推进 kMaxCatchUpFrames 帧，超过的部分会被
/// 有意丢弃（见 BattleRoom::Tick）。测试按同样的粒度推进，才与真实运行方式一致。
void AdvanceTo(RoomManager* manager, std::int64_t from_ms, std::int64_t target_ms) {
    for (std::int64_t t = from_ms + kFrameIntervalMs; t <= target_ms; t += kFrameIntervalMs) {
        manager->Tick(t);
    }
}

// ---------------------------------------------------------------------------
// 创建
// ---------------------------------------------------------------------------

TEST(RoomManagerTest, CreateReturnsRoomIdAndInitialSnapshot) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr);

    std::string room_id;
    RoomSnapshot snapshot;
    ASSERT_EQ(manager.Create("m-1", {"p-0001", "p-0002"}, kT0, &room_id, &snapshot),
              CreateOutcome::kOk);

    EXPECT_FALSE(room_id.empty());
    EXPECT_EQ(snapshot.room_id, room_id);
    EXPECT_EQ(snapshot.match_id, "m-1");
    EXPECT_EQ(snapshot.phase, RoomPhase::kCreated);
    EXPECT_EQ(manager.RoomCount(), 1U);
}

TEST(RoomManagerTest, CreateIsIdempotentByMatchId) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr);

    std::string first;
    std::string second;
    ASSERT_EQ(manager.Create("m-1", {"p-0001", "p-0002"}, kT0, &first, nullptr),
              CreateOutcome::kOk);
    ASSERT_EQ(manager.Create("m-1", {"p-0001", "p-0002"}, kT0, &second, nullptr),
              CreateOutcome::kOk);

    // 幂等的意义：Match 在调用超时后重试，不能为同一局造出第二个房间，
    // 否则玩家会被分到一个，另一个成为永远无人加入的孤儿。
    EXPECT_EQ(first, second);
    EXPECT_EQ(manager.RoomCount(), 1U);
}

TEST(RoomManagerTest, CreateRejectsInvalidInput) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr);

    std::string room_id;
    EXPECT_EQ(manager.Create("", {"p-0001"}, kT0, &room_id, nullptr),
              CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.Create("m-1", {}, kT0, &room_id, nullptr), CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.Create("m-1", {""}, kT0, &room_id, nullptr), CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.RoomCount(), 0U);
}

TEST(RoomManagerTest, RoomIdFactoryIsUsed) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });

    std::string room_id;
    ASSERT_EQ(manager.Create("m-1", {"p-0001", "p-0002"}, kT0, &room_id, nullptr),
              CreateOutcome::kOk);
    EXPECT_EQ(room_id, "r-fixed");
}

// ---------------------------------------------------------------------------
// 状态路由
// ---------------------------------------------------------------------------

TEST(RoomManagerTest, UnknownRoomIsNotFound) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr);

    RoomSnapshot snapshot;
    EXPECT_FALSE(manager.Join("r-nope", "p-0001", kT0, &snapshot).has_value());
    EXPECT_FALSE(manager.SubmitInput("r-nope", "p-0001", rgbt::room::InputKind::kAttack, &snapshot)
                     .has_value());
    EXPECT_FALSE(manager.GetState("r-nope", kT0, &snapshot));
}

TEST(RoomManagerTest, JoinAndSubmitReachTheRoom) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });

    RoomSnapshot snapshot;
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);

    EXPECT_EQ(manager.Join("r-fixed", "p-0001", kT0, &snapshot), JoinOutcome::kOk);
    EXPECT_EQ(snapshot.phase, RoomPhase::kWaiting);

    EXPECT_EQ(manager.Join("r-fixed", "p-0002", kT0, &snapshot), JoinOutcome::kOk);
    EXPECT_EQ(snapshot.phase, RoomPhase::kPlaying);

    EXPECT_EQ(manager.SubmitInput("r-fixed", "p-0001", rgbt::room::InputKind::kAttack, &snapshot),
              SubmitOutcome::kAccepted);

    EXPECT_TRUE(manager.GetState("r-fixed", kT0, &snapshot));
    EXPECT_EQ(snapshot.frame, 0);
}

// ---------------------------------------------------------------------------
// 对局结果
// ---------------------------------------------------------------------------

TEST(RoomManagerTest, ResultIsNotFoundWhileGameIsRunning) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-1", kT0, &record, nullptr), ResultOutcome::kNotFound);
}

TEST(RoomManagerTest, FinishedResultIsPersistedAndReadable) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);

    // 打满到超时：双方都没攻击，因此是平局（winner_id 为空）。
    //
    // 注意：RoomManager::Tick 在推进完房间之后会在**同一次调用内**处理待落库房间，
    // 因此到达最大帧数的那一次 Tick 就会把结果写下去，不会多等一个周期。
    AdvanceTo(&manager, kT0, kTimeoutAtMs);

    EXPECT_EQ(manager.PendingResultCount(), 0U);
    EXPECT_EQ(writer.write_calls, 1);
    ASSERT_EQ(writer.rows.count("m-1"), 1U);
    EXPECT_TRUE(writer.rows.at("m-1").winner_id.empty());

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-1", kTimeoutAtMs + kFrameIntervalMs, &record, nullptr),
              ResultOutcome::kOk);
    EXPECT_EQ(record.match_id, "m-1");
    EXPECT_EQ(record.player_count, 2);
}

TEST(RoomManagerTest, ResultIsPendingWhenStoreIsUnavailable) {
    FakeResultWriter writer;
    writer.next_write = WriteStatus::kUnavailable;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);

    AdvanceTo(&manager, kT0, kTimeoutAtMs);
    manager.Tick(kTimeoutAtMs + kFrameIntervalMs);

    // MySQL 不可用时房间停在 FINISHING，而不是假装写成功。
    EXPECT_EQ(manager.PendingResultCount(), 1U);

    MatchResultRecord record;
    RoomSnapshot snapshot;
    EXPECT_EQ(manager.GetResult("m-1", kTimeoutAtMs + kFrameIntervalMs, &record, &snapshot),
              ResultOutcome::kPending);
    // 待落库时也要给出房间快照，便于调用方判断对局是否已结束。
    EXPECT_EQ(snapshot.phase, RoomPhase::kFinishing);
}

TEST(RoomManagerTest, ResultIsPersistedAfterStoreRecovers) {
    FakeResultWriter writer;
    writer.next_write = WriteStatus::kUnavailable;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);

    AdvanceTo(&manager, kT0, kTimeoutAtMs);
    manager.Tick(kTimeoutAtMs + kFrameIntervalMs);
    ASSERT_EQ(manager.PendingResultCount(), 1U);

    // 存储恢复。下一次重试（间隔 1 秒）应当写入成功。
    writer.next_write = WriteStatus::kOk;
    manager.Tick(kTimeoutAtMs + kFrameIntervalMs + 1000);

    EXPECT_EQ(manager.PendingResultCount(), 0U);
    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-1", kTimeoutAtMs + kFrameIntervalMs + 1000, &record, nullptr),
              ResultOutcome::kOk);
}

TEST(RoomManagerTest, RepeatedFinishDoesNotWriteTwice) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);

    AdvanceTo(&manager, kT0, kTimeoutAtMs);
    manager.Tick(kTimeoutAtMs + kFrameIntervalMs);
    // 再推很多次 Tick，不应产生第二次写入。
    for (int i = 0; i < 5; ++i) {
        manager.Tick(kTimeoutAtMs + (i + 2) * kFrameIntervalMs);
    }

    EXPECT_EQ(writer.write_calls, 1);
    EXPECT_EQ(writer.rows.size(), 1U);
}

TEST(RoomManagerTest, StoreUnavailableOnReadIsNotReportedAsNotFound) {
    FakeResultWriter writer;
    writer.next_read = ReadStatus::kUnavailable;
    RoomManager manager(&writer, nullptr);

    MatchResultRecord record;
    // 「存储挂了」与「没有这条结果」必须分开：前者可重试，后者是 404。
    EXPECT_EQ(manager.GetResult("m-missing", kT0, &record, nullptr), ResultOutcome::kUnavailable);
}

TEST(RoomManagerTest, UnknownMatchIdIsNotFoundWhenStoreSaysSo) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr);

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-missing", kT0, &record, nullptr), ResultOutcome::kNotFound);
}

// ---------------------------------------------------------------------------
// 回收
// ---------------------------------------------------------------------------

TEST(RoomManagerTest, AbortedRoomIsReapedAfterRetention) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);

    // 没人加入 -> 等待超时 -> ABORTED。
    const std::int64_t abort_at = kT0 + kWaitingTimeoutMs;
    manager.Tick(abort_at);
    ASSERT_EQ(manager.RoomCount(), 1U);

    // 保留期结束前仍在，之后被回收。
    manager.Tick(abort_at + kFinishedRetentionMs - 1);
    EXPECT_EQ(manager.RoomCount(), 1U);

    manager.Tick(abort_at + kFinishedRetentionMs);
    EXPECT_EQ(manager.RoomCount(), 0U);
}

TEST(RoomManagerTest, FinishedRoomIsReapedAndResultStillReadableFromStore) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);

    AdvanceTo(&manager, kT0, kTimeoutAtMs);
    manager.Tick(kTimeoutAtMs + kFrameIntervalMs);

    // 房间被回收后，历史对局仍然可查——这是「结果已落库」而非「只存在内存里」的证据。
    manager.Tick(kTimeoutAtMs + kFinishedRetentionMs + 1);
    EXPECT_EQ(manager.RoomCount(), 0U);

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-1", kTimeoutAtMs + kFinishedRetentionMs + 2, &record, nullptr),
              ResultOutcome::kOk);
    EXPECT_EQ(record.match_id, "m-1");
}

TEST(RoomManagerTest, PlayingCountTracksActiveRooms) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    EXPECT_EQ(manager.PlayingCount(), 0U);

    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);
    EXPECT_EQ(manager.PlayingCount(), 1U);
}

// ---------------------------------------------------------------------------
// 房间快照写入（TASK-013）
//
// 这组用例的关键不是"写进去了"，而是**失败时会发生什么**：
// 快照可以丢弃，所以写失败必须不重试、不阻塞、不影响对局推进。
// 那条路径只有在单元测试里才能被稳定构造。
// ---------------------------------------------------------------------------

/// 内存快照存储。以 match_id 为键，因此可以验证"每个房间只保留最新一份"。
class FakeSnapshotWriter : public RoomSnapshotWriter {
public:
    /// 下一次（以及之后所有）写入的返回值。
    SnapshotWriteStatus next_write = SnapshotWriteStatus::kOk;

    std::map<std::string, RoomSnapshotRecord> rows;
    int write_calls = 0;

    SnapshotWriteStatus Save(const RoomSnapshotRecord& record) override {
        ++write_calls;
        if (next_write != SnapshotWriteStatus::kOk) {
            return next_write;
        }
        rows[record.match_id] = record;
        return SnapshotWriteStatus::kOk;
    }

    bool IsHealthy() override { return true; }
};

TEST(RoomManagerTest, SnapshotIsWrittenImmediatelyOnCreate) {
    // 第一个快照不等间隔：`rooms` 表的用途之一是"事后能查到这里曾经有一局"。
    // 若等满一个间隔再写，一个刚创建就异常退出的房间会完全消失。
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    RoomManager manager(&writer, &snapshots, []() { return std::string("r-fixed"); });

    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Tick(kT0);

    ASSERT_EQ(snapshots.rows.count("m-1"), 1U);
    const RoomSnapshotRecord& record = snapshots.rows.at("m-1");
    EXPECT_EQ(record.room_id, "r-fixed");
    EXPECT_EQ(record.phase, RoomPhase::kCreated);
    EXPECT_EQ(record.frame, 0);
    EXPECT_EQ(record.snapshot_at_ms, kT0);
    ASSERT_EQ(record.players.size(), 2U);
    EXPECT_EQ(record.players[0].player_id, "p-0001");
    EXPECT_FALSE(record.players[0].joined);
}

TEST(RoomManagerTest, SnapshotIsWrittenAtIntervalNotEveryTick) {
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    RoomManager manager(&writer, &snapshots, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Tick(kT0);
    const int after_first = snapshots.write_calls;
    ASSERT_EQ(after_first, 1);

    // 间隔之内反复 Tick 不应产生新快照。房间的推进线程是 50 ms 一次，
    // 若每个 Tick 都写库，压力会是不必要的 20 倍。
    for (std::int64_t t = kT0 + kFrameIntervalMs; t < kT0 + kSnapshotIntervalMs;
         t += kFrameIntervalMs) {
        manager.Tick(t);
    }
    EXPECT_EQ(snapshots.write_calls, after_first);

    // 到达间隔后写一次。
    manager.Tick(kT0 + kSnapshotIntervalMs);
    EXPECT_GT(snapshots.write_calls, after_first);
}

TEST(RoomManagerTest, SnapshotWriteFailureDoesNotBlockTheGame) {
    // 这是本任务最重要的区分：**快照可以丢弃**。
    // 写失败必须不重试、不阻塞，对局照常推进。
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    snapshots.next_write = SnapshotWriteStatus::kUnavailable;
    RoomManager manager(&writer, &snapshots, []() { return std::string("r-fixed"); });

    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);

    // 推进 20 帧，期间每一次快照尝试都失败。
    manager.Tick(kT0);
    AdvanceTo(&manager, kT0, kT0 + 20 * kFrameIntervalMs);

    EXPECT_GT(manager.SnapshotFailureCount(), 0U);
    EXPECT_EQ(manager.SnapshotWriteCount(), 0U);
    // 对局没有被拖住：帧号照常前进。
    RoomSnapshot snapshot;
    ASSERT_TRUE(manager.GetState("r-fixed", kT0 + 20 * kFrameIntervalMs, &snapshot));
    EXPECT_EQ(snapshot.frame, 20);

    // 存储恢复后，下一个快照周期自然写成功——不需要任何补写逻辑。
    snapshots.next_write = SnapshotWriteStatus::kOk;
    manager.Tick(kT0 + 21 * kFrameIntervalMs + kSnapshotIntervalMs);
    EXPECT_GT(manager.SnapshotWriteCount(), 0U);
}

TEST(RoomManagerTest, TerminalSnapshotIsWrittenOnFinish) {
    // 结束后必须立刻落一次终态，否则表里会长期停在 FINISHING，
    // 事后核对该房间时会得到一个"好像还没写完"的错误印象。
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    RoomManager manager(&writer, &snapshots, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);
    manager.Tick(kT0);

    // 打满到超时结束（双方都不攻击，因此是平局）。
    AdvanceTo(&manager, kT0, kTimeoutAtMs);
    // 结果在到达 kTimeoutAtMs 的那次 Tick 内同步落库，房间在同一次调用里从
    // FINISHING 变成 FINISHED。**终态快照要等下一次 Tick 才写**：
    // 那一次的快照内容显示 FINISHING 是准确的（结果确实还没写完），
    // 硬要它显示 FINISHED 就等于让快照谎报结果已落库。
    manager.Tick(kTimeoutAtMs + 1);

    ASSERT_EQ(snapshots.rows.count("m-1"), 1U);
    const RoomSnapshotRecord& record = snapshots.rows.at("m-1");
    EXPECT_EQ(record.phase, RoomPhase::kFinished);
    EXPECT_TRUE(record.winner_id.empty());
    EXPECT_EQ(record.frame, kMaxFrames);
}

TEST(RoomManagerTest, NoSnapshotWriterMeansNoWrites) {
    // 不注入快照写入器时必须安全：既不能崩，也不能偷偷写什么。
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);
    AdvanceTo(&manager, kT0, kT0 + 50 * kFrameIntervalMs);

    EXPECT_EQ(manager.SnapshotWriteCount(), 0U);
    EXPECT_EQ(manager.SnapshotFailureCount(), 0U);
}

TEST(RoomManagerTest, SnapshotContentTracksHpAndJoinState) {
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    RoomManager manager(&writer, &snapshots, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);
    manager.SubmitInput("r-fixed", "p-0001", rgbt::room::InputKind::kAttack, nullptr);

    // 推进两帧让攻击结算，再到快照间隔落一次。
    AdvanceTo(&manager, kT0, kT0 + kSnapshotIntervalMs);

    ASSERT_EQ(snapshots.rows.count("m-1"), 1U);
    const RoomSnapshotRecord& record = snapshots.rows.at("m-1");
    EXPECT_EQ(record.phase, RoomPhase::kPlaying);
    EXPECT_TRUE(record.players[0].joined);
    EXPECT_TRUE(record.players[1].joined);
    // p-0001 攻击 p-0002，因此下标 1 掉血。
    EXPECT_EQ(record.players[1].hp, kInitialHp - rgbt::room::kAttackDamage);
}

TEST(RoomManagerTest, ReapedRoomClearsSnapshotState) {
    // 房间被回收后，它的快照调度状态也必须清掉，否则这张表会随对局数无限增长，
    // 而且残留状态会让后续同 match_id 的房间以为"已经写过快照"。
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    RoomManager manager(&writer, &snapshots, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);
    AdvanceTo(&manager, kT0, kTimeoutAtMs);
    ASSERT_EQ(manager.RoomCount(), 1U);

    // 超过保留期后房间被回收。
    manager.Tick(kTimeoutAtMs + kFinishedRetentionMs + 1);
    EXPECT_EQ(manager.RoomCount(), 0U);

    // 重建同一 match_id：应当被当作全新房间，因此**立刻**再写一次快照。
    const int before = snapshots.write_calls;
    manager.Create("m-1", {"p-0003", "p-0004"}, kT0, nullptr, nullptr);
    manager.Tick(kT0 + 1);
    EXPECT_GT(snapshots.write_calls, before);
    EXPECT_EQ(snapshots.rows.at("m-1").players[0].player_id, "p-0003");
}

}  // namespace
