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
#include "room_snapshot_reader.hpp"
#include "room_snapshot_writer.hpp"
#include "room_types.hpp"

namespace {

using rgbt::room::CreateOutcome;
using rgbt::room::FinishReason;
using rgbt::room::JoinOutcome;
using rgbt::room::kAttackDamage;
using rgbt::room::kFinishedRetentionMs;
using rgbt::room::kFrameIntervalMs;
using rgbt::room::kInitialHp;
using rgbt::room::kMaxFrames;
using rgbt::room::kSnapshotIntervalMs;
using rgbt::room::kWaitingTimeoutMs;
using rgbt::room::MatchResultRecord;
using rgbt::room::MatchResultWriter;
using rgbt::room::ParseFinishReason;
using rgbt::room::ParseRoomPhase;
using rgbt::room::ReadStatus;
using rgbt::room::RestoreReport;
using rgbt::room::ResultOutcome;
using rgbt::room::RoomManager;
using rgbt::room::RoomPhase;
using rgbt::room::RoomPlayerRecord;
using rgbt::room::RoomSnapshot;
using rgbt::room::RoomSnapshotReader;
using rgbt::room::RoomSnapshotRecord;
using rgbt::room::RoomSnapshotRow;
using rgbt::room::RoomSnapshotWriter;
using rgbt::room::SnapshotWriteStatus;
using rgbt::room::SubmitOutcome;
using rgbt::room::ValidateRoomSnapshot;
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
    RoomManager manager(&writer, nullptr, nullptr);

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
    RoomManager manager(&writer, nullptr, nullptr);

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
    RoomManager manager(&writer, nullptr, nullptr);

    std::string room_id;
    EXPECT_EQ(manager.Create("", {"p-0001"}, kT0, &room_id, nullptr),
              CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.Create("m-1", {}, kT0, &room_id, nullptr), CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.Create("m-1", {""}, kT0, &room_id, nullptr), CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.RoomCount(), 0U);
}

TEST(RoomManagerTest, RoomIdFactoryIsUsed) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });

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
    RoomManager manager(&writer, nullptr, nullptr);

    RoomSnapshot snapshot;
    EXPECT_FALSE(manager.Join("r-nope", "p-0001", kT0, &snapshot).has_value());
    EXPECT_FALSE(manager.SubmitInput("r-nope", "p-0001", rgbt::room::InputKind::kAttack, &snapshot)
                     .has_value());
    EXPECT_FALSE(manager.GetState("r-nope", kT0, &snapshot));
}

TEST(RoomManagerTest, JoinAndSubmitReachTheRoom) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });

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
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-1", kT0, &record, nullptr), ResultOutcome::kNotFound);
}

TEST(RoomManagerTest, FinishedResultIsPersistedAndReadable) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, nullptr, nullptr);

    MatchResultRecord record;
    // 「存储挂了」与「没有这条结果」必须分开：前者可重试，后者是 404。
    EXPECT_EQ(manager.GetResult("m-missing", kT0, &record, nullptr), ResultOutcome::kUnavailable);
}

TEST(RoomManagerTest, UnknownMatchIdIsNotFoundWhenStoreSaysSo) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, nullptr);

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-missing", kT0, &record, nullptr), ResultOutcome::kNotFound);
}

// ---------------------------------------------------------------------------
// 回收
// ---------------------------------------------------------------------------

TEST(RoomManagerTest, AbortedRoomIsReapedAfterRetention) {
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, &snapshots, nullptr, []() { return std::string("r-fixed"); });

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
    RoomManager manager(&writer, &snapshots, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, &snapshots, nullptr, []() { return std::string("r-fixed"); });

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
    RoomManager manager(&writer, &snapshots, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, &snapshots, nullptr, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, &snapshots, nullptr, []() { return std::string("r-fixed"); });
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

// ---------------------------------------------------------------------------
// 启动恢复（TASK-014）
//
// 这组用例覆盖的是恢复路径上最容易出错、又最难在端到端里构造的部分：
// 损坏快照的处置、时间基准的重置、FINISHING 的立即重试。
// ---------------------------------------------------------------------------

/// 可控的快照读取器。
class FakeSnapshotReader : public RoomSnapshotReader {
public:
    /// 下一次 LoadUnfinished 是否失败（模拟存储不可用）。
    bool fail_load = false;
    /// 要返回的行。
    std::vector<RoomSnapshotRow> rows;
    int load_calls = 0;

    bool LoadUnfinished(std::vector<RoomSnapshotRow>* out_rows) override {
        ++load_calls;
        if (fail_load) {
            return false;
        }
        if (out_rows != nullptr) {
            *out_rows = rows;
        }
        return true;
    }

    bool IsHealthy() override { return !fail_load; }
};

/// 构造一条可用的快照记录。
RoomSnapshotRecord MakeRecord(RoomPhase phase, std::int64_t frame, std::int64_t snapshot_at_ms) {
    RoomSnapshotRecord record;
    record.match_id = "m-1";
    record.room_id = "r-1";
    record.phase = phase;
    record.frame = frame;
    record.players[0] = RoomPlayerRecord{"p-0001", kInitialHp, true};
    record.players[1] = RoomPlayerRecord{"p-0002", kInitialHp - kAttackDamage, true};
    record.started_at_ms = snapshot_at_ms - 1000;
    record.snapshot_at_ms = snapshot_at_ms;
    return record;
}

TEST(RoomManagerTest, RestoreIsNoOpWithoutReader) {
    // 不注入读取器时必须安全：不恢复、不报错、不崩。测试与"不关心持久化"的部署走这里。
    FakeResultWriter writer;
    RoomManager manager(&writer, nullptr, nullptr, []() { return std::string("r-fixed"); });

    const RestoreReport report = manager.Restore(kT0);
    EXPECT_EQ(report.scanned, 0U);
    EXPECT_EQ(report.restored, 0U);
    EXPECT_EQ(manager.RoomCount(), 0U);
}

TEST(RoomManagerTest, RestoreReportsLoadFailureAndRecoversNothing) {
    // 存储不可用时**不恢复任何房间**，并如实报告。
    // "以为恢复了其实没有"比"空手启动"危险得多。
    FakeResultWriter writer;
    FakeSnapshotReader reader;
    reader.fail_load = true;
    reader.rows.push_back(RoomSnapshotRow{MakeRecord(RoomPhase::kPlaying, 30, kT0), ""});
    RoomManager manager(&writer, nullptr, &reader, []() { return std::string("r-fixed"); });

    const RestoreReport report = manager.Restore(kT0 + 1);
    EXPECT_TRUE(report.load_failed);
    EXPECT_EQ(report.restored, 0U);
    EXPECT_EQ(manager.RoomCount(), 0U);
}

TEST(RoomManagerTest, RestoreRebuildsPlayingRoomWithoutCatchingUpFrames) {
    // 本任务最关键的一条边界：重启后房间回到**快照那一刻**的帧号，
    // 停机期间本应推进的帧被丢弃，而不是在启动瞬间一次性补上。
    FakeResultWriter writer;
    FakeSnapshotReader reader;
    const std::int64_t snapshot_at = kT0;
    // 快照是 30 帧（3 秒）时的状态；停机 10 分钟后再启动。
    reader.rows.push_back(RoomSnapshotRow{MakeRecord(RoomPhase::kPlaying, 30, snapshot_at), ""});
    RoomManager manager(&writer, nullptr, &reader, []() { return std::string("r-fixed"); });

    const std::int64_t restart_at = snapshot_at + 600'000;
    const RestoreReport report = manager.Restore(restart_at);
    EXPECT_EQ(report.scanned, 1U);
    EXPECT_EQ(report.restored, 1U);
    EXPECT_EQ(report.rejected, 0U);

    RoomSnapshot snapshot;
    // 注意房间号来自快照本身（"r-1"），**不是** room_id_factory 的产物：
    // Restore 不生成房间号，它只是把已经存在的房间装回内存。
    ASSERT_TRUE(manager.GetState("r-1", restart_at, &snapshot));
    EXPECT_EQ(snapshot.phase, RoomPhase::kPlaying);
    // 帧号停在快照点，没有补上停机期间的 6000 帧。
    EXPECT_EQ(snapshot.frame, 30);
    // 血量与加入状态都精确恢复。
    EXPECT_EQ(snapshot.players[0].hp, kInitialHp);
    EXPECT_EQ(snapshot.players[1].hp, kInitialHp - kAttackDamage);
    EXPECT_TRUE(snapshot.players[0].connected);

    // 重启后正常推进：一个帧长之后前进一帧。
    manager.Tick(restart_at + kFrameIntervalMs);
    ASSERT_TRUE(manager.GetState("r-1", restart_at + kFrameIntervalMs, &snapshot));
    EXPECT_EQ(snapshot.frame, 31);
}

TEST(RoomManagerTest, RestorePopulatesMatchIndexSoCreateStaysIdempotent) {
    // 恢复必须同时填 match_index_，否则 Match 超时重试 CreateRoom 时会
    // 为同一 match_id 造出第二个房间（TASK-008 花力气保证的幂等会在这里破功）。
    FakeResultWriter writer;
    FakeSnapshotReader reader;
    reader.rows.push_back(RoomSnapshotRow{MakeRecord(RoomPhase::kPlaying, 5, kT0), ""});
    RoomManager manager(&writer, nullptr, &reader, []() { return std::string("r-fixed"); });
    ASSERT_EQ(manager.Restore(kT0).restored, 1U);

    std::string out_room_id;
    const CreateOutcome outcome =
        manager.Create("m-1", {"p-0001", "p-0002"}, kT0, &out_room_id, nullptr);
    EXPECT_EQ(outcome, CreateOutcome::kOk);
    EXPECT_EQ(out_room_id, "r-1");
    // 仍然只有一个房间。
    EXPECT_EQ(manager.RoomCount(), 1U);
}

TEST(RoomManagerTest, RestoreRejectsCorruptSnapshotAndMarksItAborted) {
    // 损坏快照**不静默丢弃**：记下原因，并把那一行改写成 ABORTED，
    // 否则它会永远停在 playing，下次启动又被扫出来、又被拒绝。
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    FakeSnapshotReader reader;
    RoomSnapshotRow bad;
    bad.record = MakeRecord(RoomPhase::kPlaying, 5, kT0);
    bad.problem = "第 1 位玩家血量超出 [0, 100]";
    reader.rows.push_back(bad);
    RoomManager manager(&writer, &snapshots, &reader, []() { return std::string("r-fixed"); });

    const RestoreReport report = manager.Restore(kT0 + 100);
    EXPECT_EQ(report.scanned, 1U);
    EXPECT_EQ(report.restored, 0U);
    EXPECT_EQ(report.rejected, 1U);
    EXPECT_EQ(manager.RoomCount(), 0U);

    // 被改写成 ABORTED 并落库。
    ASSERT_EQ(snapshots.rows.count("m-1"), 1U);
    EXPECT_EQ(snapshots.rows.at("m-1").phase, RoomPhase::kAborted);
    EXPECT_EQ(snapshots.rows.at("m-1").finish_reason, rgbt::room::FinishReason::kAborted);
    // snapshot_at_ms 被更新为本次时刻，否则写回会被"新不旧于旧"的守卫拒掉。
    EXPECT_EQ(snapshots.rows.at("m-1").snapshot_at_ms, kT0 + 100);
}

TEST(RoomManagerTest, RestoreRejectsRecordThatCannotBuildARoom) {
    // 这条用例**故意让假读取器跳过校验**（problem 留空），以触达 Restore 的
    // 第二道防线：真实读取器会在返回前调用 ValidateRoomSnapshot，因此
    // 正常路径下走不到这里；万一将来有人新增一个不做校验的读取实现，
    // 这一层仍然不会把"两个位置是同一个人"的房间装进内存。
    FakeResultWriter writer;
    FakeSnapshotWriter snapshots;
    FakeSnapshotReader reader;
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kPlaying, 5, kT0);
    record.players[1].player_id = record.players[0].player_id;
    reader.rows.push_back(RoomSnapshotRow{record, ""});
    RoomManager manager(&writer, &snapshots, &reader, []() { return std::string("r-fixed"); });

    const RestoreReport report = manager.Restore(kT0 + 100);
    EXPECT_EQ(report.rejected, 1U);
    EXPECT_EQ(report.restored, 0U);
    ASSERT_EQ(snapshots.rows.count("m-1"), 1U);
    EXPECT_EQ(snapshots.rows.at("m-1").phase, RoomPhase::kAborted);
}

TEST(RoomManagerTest, RestoreRevivesFinishingRoomAndPersistsResultImmediately) {
    // TASK-008 留下的已知限制：已结束但未落库的对局重启即丢失。
    // 恢复路径必须把 FINISHING 房间重新纳入落库重试，而且**立即**到期——
    // 结果已经在内存里消失过一次，再等一个完整重试间隔没有意义。
    FakeResultWriter writer;
    FakeSnapshotReader reader;
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kFinishing, kMaxFrames, kT0);
    record.finish_reason = rgbt::room::FinishReason::kHpZero;
    record.winner_id = "p-0001";
    record.finished_at_ms = kT0;
    reader.rows.push_back(RoomSnapshotRow{record, ""});

    RoomManager manager(&writer, nullptr, &reader, []() { return std::string("r-fixed"); });
    const std::int64_t restart_at = kT0 + 60'000;
    ASSERT_EQ(manager.Restore(restart_at).restored, 1U);
    EXPECT_EQ(manager.PendingResultCount(), 1U);

    // 第一次 Tick 就应当尝试落库，不等待 kResultRetryIntervalMs。
    manager.Tick(restart_at);
    EXPECT_EQ(writer.write_calls, 1);
    ASSERT_EQ(writer.rows.count("m-1"), 1U);
    EXPECT_EQ(writer.rows.at("m-1").winner_id, "p-0001");
    EXPECT_EQ(writer.rows.at("m-1").finished_at_ms, kT0);
}

TEST(RoomManagerTest, RestoreRevivesWaitingRoom) {
    // CREATED / WAITING 也恢复：等待中的玩家可以在 Room 重启后继续加入。
    // 等待超时用快照时刻近似（表里没有 created_at_ms），因此这里断言的是
    // "不会立刻被当成超时废掉"。
    FakeResultWriter writer;
    FakeSnapshotReader reader;
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kWaiting, 0, kT0);
    record.players[0].joined = true;
    record.players[1].joined = false;
    reader.rows.push_back(RoomSnapshotRow{record, ""});
    RoomManager manager(&writer, nullptr, &reader, []() { return std::string("r-fixed"); });

    const std::int64_t restart_at = kT0 + 5000;
    ASSERT_EQ(manager.Restore(restart_at).restored, 1U);

    RoomSnapshot snapshot;
    ASSERT_TRUE(manager.GetState("r-1", restart_at, &snapshot));
    EXPECT_EQ(snapshot.phase, RoomPhase::kWaiting);
    // 第二个玩家仍可加入并开局。
    EXPECT_EQ(manager.Join("r-1", "p-0002", restart_at, &snapshot), JoinOutcome::kOk);
    EXPECT_EQ(snapshot.phase, RoomPhase::kPlaying);
}

// ---------------------------------------------------------------------------
// 快照校验（纯函数，因此可以逐条构造损坏形态）
// ---------------------------------------------------------------------------

TEST(RoomSnapshotValidationTest, AcceptsAWellFormedRecord) {
    EXPECT_EQ(ValidateRoomSnapshot(MakeRecord(RoomPhase::kPlaying, 10, kT0)), "");
}

TEST(RoomSnapshotValidationTest, RejectsUnknownStateStrings) {
    // 未知状态必须解析失败，而不是退回默认值——把未知状态当成 CREATED
    // 会让一条损坏的快照被当成正常房间恢复出来。
    EXPECT_FALSE(ParseRoomPhase("PLAYING").has_value());
    EXPECT_FALSE(ParseRoomPhase("").has_value());
    EXPECT_TRUE(ParseRoomPhase("playing").has_value());
    EXPECT_FALSE(ParseFinishReason("nope").has_value());
    EXPECT_TRUE(ParseFinishReason("hp_zero").has_value());
}

TEST(RoomSnapshotValidationTest, RejectsOutOfRangeValues) {
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kPlaying, 10, kT0);
    record.frame = kMaxFrames + 1;
    EXPECT_NE(ValidateRoomSnapshot(record), "");

    record = MakeRecord(RoomPhase::kPlaying, 10, kT0);
    record.players[0].hp = kInitialHp + 1;
    EXPECT_NE(ValidateRoomSnapshot(record), "");

    record = MakeRecord(RoomPhase::kPlaying, 10, kT0);
    record.players[1].hp = -1;
    EXPECT_NE(ValidateRoomSnapshot(record), "");
}

TEST(RoomSnapshotValidationTest, RejectsEmptyAndDuplicatePlayerIds) {
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kPlaying, 10, kT0);
    record.players[1].player_id = "";
    EXPECT_NE(ValidateRoomSnapshot(record), "");

    record = MakeRecord(RoomPhase::kPlaying, 10, kT0);
    record.players[1].player_id = record.players[0].player_id;
    EXPECT_NE(ValidateRoomSnapshot(record), "");
}

TEST(RoomSnapshotValidationTest, RejectsPlayedRoomWhereNobodyJoined) {
    // "对局状态为已开打，但双方都不在房间内"是任务单点名的失败场景。
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kPlaying, 10, kT0);
    record.players[0].joined = false;
    record.players[1].joined = false;
    EXPECT_NE(ValidateRoomSnapshot(record), "");

    // 但同样的"没人加入"在 WAITING 阶段是正常状态，不是损坏。
    RoomSnapshotRecord waiting = MakeRecord(RoomPhase::kWaiting, 0, kT0);
    waiting.players[0].joined = false;
    waiting.players[1].joined = false;
    EXPECT_EQ(ValidateRoomSnapshot(waiting), "");
}

TEST(RoomSnapshotValidationTest, RejectsWinnerNotOnTheField) {
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kFinishing, kMaxFrames, kT0);
    record.finish_reason = rgbt::room::FinishReason::kHpZero;
    record.winner_id = "p-9999";
    EXPECT_NE(ValidateRoomSnapshot(record), "");

    record.winner_id = "p-0001";
    EXPECT_EQ(ValidateRoomSnapshot(record), "");
}

TEST(RoomSnapshotValidationTest, RejectsTerminalStateWithoutFinishReason) {
    // 已处于终态却没有结束原因，说明两列不是同一次写入的结果。
    RoomSnapshotRecord record = MakeRecord(RoomPhase::kFinished, kMaxFrames, kT0);
    record.finish_reason = rgbt::room::FinishReason::kNone;
    EXPECT_NE(ValidateRoomSnapshot(record), "");
}

}  // namespace
