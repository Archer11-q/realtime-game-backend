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
#include "room_types.hpp"

namespace {

using rgbt::room::CreateOutcome;
using rgbt::room::JoinOutcome;
using rgbt::room::kFinishedRetentionMs;
using rgbt::room::kFrameIntervalMs;
using rgbt::room::kInitialHp;
using rgbt::room::kMaxFrames;
using rgbt::room::kWaitingTimeoutMs;
using rgbt::room::MatchResultRecord;
using rgbt::room::MatchResultWriter;
using rgbt::room::ReadStatus;
using rgbt::room::ResultOutcome;
using rgbt::room::RoomManager;
using rgbt::room::RoomPhase;
using rgbt::room::RoomSnapshot;
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
    RoomManager manager(&writer);

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
    RoomManager manager(&writer);

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
    RoomManager manager(&writer);

    std::string room_id;
    EXPECT_EQ(manager.Create("", {"p-0001"}, kT0, &room_id, nullptr),
              CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.Create("m-1", {}, kT0, &room_id, nullptr), CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.Create("m-1", {""}, kT0, &room_id, nullptr), CreateOutcome::kInvalidArgument);
    EXPECT_EQ(manager.RoomCount(), 0U);
}

TEST(RoomManagerTest, RoomIdFactoryIsUsed) {
    FakeResultWriter writer;
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });

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
    RoomManager manager(&writer);

    RoomSnapshot snapshot;
    EXPECT_FALSE(manager.Join("r-nope", "p-0001", kT0, &snapshot).has_value());
    EXPECT_FALSE(manager.SubmitInput("r-nope", "p-0001", rgbt::room::InputKind::kAttack, &snapshot)
                     .has_value());
    EXPECT_FALSE(manager.GetState("r-nope", kT0, &snapshot));
}

TEST(RoomManagerTest, JoinAndSubmitReachTheRoom) {
    FakeResultWriter writer;
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });

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
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-1", kT0, &record, nullptr), ResultOutcome::kNotFound);
}

TEST(RoomManagerTest, FinishedResultIsPersistedAndReadable) {
    FakeResultWriter writer;
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer);

    MatchResultRecord record;
    // 「存储挂了」与「没有这条结果」必须分开：前者可重试，后者是 404。
    EXPECT_EQ(manager.GetResult("m-missing", kT0, &record, nullptr), ResultOutcome::kUnavailable);
}

TEST(RoomManagerTest, UnknownMatchIdIsNotFoundWhenStoreSaysSo) {
    FakeResultWriter writer;
    RoomManager manager(&writer);

    MatchResultRecord record;
    EXPECT_EQ(manager.GetResult("m-missing", kT0, &record, nullptr), ResultOutcome::kNotFound);
}

// ---------------------------------------------------------------------------
// 回收
// ---------------------------------------------------------------------------

TEST(RoomManagerTest, AbortedRoomIsReapedAfterRetention) {
    FakeResultWriter writer;
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
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
    RoomManager manager(&writer, []() { return std::string("r-fixed"); });
    manager.Create("m-1", {"p-0001", "p-0002"}, kT0, nullptr, nullptr);
    EXPECT_EQ(manager.PlayingCount(), 0U);

    manager.Join("r-fixed", "p-0001", kT0, nullptr);
    manager.Join("r-fixed", "p-0002", kT0, nullptr);
    EXPECT_EQ(manager.PlayingCount(), 1U);
}

}  // namespace
