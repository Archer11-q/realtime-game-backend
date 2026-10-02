/// @file battle_room_test.cpp
/// @brief BattleRoom 的单元测试：生命周期、输入结算与胜负判定。
///
/// 时间全部由用例注入（Tick(now_ms)），因此「HP 归零」「达到最大帧数」
/// 「等待加入超时」这些路径可以精确验证，不需要 sleep，也不会偶发失败。

#include "battle_room.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "room_types.hpp"

namespace {

using rgbt::room::BattleRoom;
using rgbt::room::FinishReason;
using rgbt::room::InputKind;
using rgbt::room::JoinOutcome;
using rgbt::room::kAttackDamage;
using rgbt::room::kFrameIntervalMs;
using rgbt::room::kInitialHp;
using rgbt::room::kMaxFrames;
using rgbt::room::kMaxSnapshotHistory;
using rgbt::room::kWaitingTimeoutMs;
using rgbt::room::PlayerSnapshot;
using rgbt::room::PresenceOutcome;
using rgbt::room::RoomPhase;
using rgbt::room::RoomSnapshot;
using rgbt::room::SubmitOutcome;

/// 时间基准。用例都在它之上加减，避免依赖真实时钟。
constexpr std::int64_t kT0 = 1'700'000'000'000LL;

/// 找到某个玩家的血量。找不到返回 -1，便于断言中一眼看出问题。
std::int32_t HpOf(const RoomSnapshot& snapshot, const std::string& player_id) {
    for (const PlayerSnapshot& player : snapshot.players) {
        if (player.player_id == player_id) {
            return player.hp;
        }
    }
    return -1;
}

/// 构造并让双方加入，返回处于 PLAYING 的房间。
///
/// BattleRoom 不可拷贝，因此这里用工厂函数直接构造并依赖返回值优化。
BattleRoom PlayingRoom() {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    room.Join("p-0001", kT0);
    room.Join("p-0002", kT0);
    return room;
}

/// 到达最大帧数、按 HP 判定胜负的时刻。
constexpr std::int64_t kTimeoutAtMs = kT0 + kMaxFrames * kFrameIntervalMs;

/// 从 from_ms 按帧推进到 target_ms。
///
/// **必须分多次调用**：单次 Tick 最多推进 kMaxCatchUpFrames 帧，超过的部分会被
/// 有意丢弃（见 BattleRoom::Tick 的注释）。因此测试也必须按同样的粒度推进，
/// 不能一步跳到最后——那测的就不是真实运行方式了。
void AdvanceTo(BattleRoom* room, std::int64_t from_ms, std::int64_t target_ms) {
    for (std::int64_t t = from_ms + kFrameIntervalMs; t <= target_ms; t += kFrameIntervalMs) {
        room->Tick(t);
    }
}

/// 从开局推进到最大帧数。
void RunToTimeout(BattleRoom* room) {
    AdvanceTo(room, kT0, kTimeoutAtMs);
}

// ---------------------------------------------------------------------------
// 创建与加入
// ---------------------------------------------------------------------------

TEST(BattleRoomTest, NewRoomIsCreatedWithFullHpAndNobodyJoined) {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    const RoomSnapshot snapshot = room.Snapshot();

    EXPECT_EQ(snapshot.phase, RoomPhase::kCreated);
    EXPECT_EQ(snapshot.room_id, "r-1");
    EXPECT_EQ(snapshot.match_id, "m-1");
    EXPECT_EQ(snapshot.frame, 0);
    ASSERT_EQ(snapshot.players.size(), 2U);
    for (const PlayerSnapshot& player : snapshot.players) {
        EXPECT_EQ(player.hp, kInitialHp);
        // 还没加入房间，因此 connected 为 false。TASK-008 用这个字段表达
        // 「当前在房间内」，而不是「网络是否连通」。
        EXPECT_FALSE(player.connected);
    }
}

TEST(BattleRoomTest, FirstJoinMovesToWaiting) {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    EXPECT_EQ(room.Join("p-0001", kT0), JoinOutcome::kOk);
    EXPECT_EQ(room.phase(), RoomPhase::kWaiting);
}

TEST(BattleRoomTest, SecondJoinStartsTheGame) {
    BattleRoom room = PlayingRoom();
    const RoomSnapshot snapshot = room.Snapshot();

    EXPECT_EQ(snapshot.phase, RoomPhase::kPlaying);
    EXPECT_EQ(snapshot.started_at_ms, kT0);
    EXPECT_EQ(snapshot.frame, 0);
    for (const PlayerSnapshot& player : snapshot.players) {
        EXPECT_TRUE(player.connected);
    }
}

TEST(BattleRoomTest, DuplicateJoinIsIdempotent) {
    BattleRoom room = PlayingRoom();
    // 重复加入按幂等成功处理，不改变状态，也不把人重复放进位置。
    EXPECT_EQ(room.Join("p-0001", kT0), JoinOutcome::kOk);
    EXPECT_EQ(room.phase(), RoomPhase::kPlaying);
    EXPECT_EQ(room.Snapshot().players.size(), 2U);
}

TEST(BattleRoomTest, StrangerCannotJoin) {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    // 房间有空位，但那不是给这个人的。
    EXPECT_EQ(room.Join("p-9999", kT0), JoinOutcome::kNotAMember);
}

TEST(BattleRoomTest, EmptyPlayerIdIsRejected) {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    EXPECT_EQ(room.Join("", kT0), JoinOutcome::kInvalidArgument);
}

// ---------------------------------------------------------------------------
// 输入与伤害结算
// ---------------------------------------------------------------------------

TEST(BattleRoomTest, InputBeforeStartIsRejected) {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    room.Join("p-0001", kT0);
    EXPECT_EQ(room.SubmitInput("p-0001", InputKind::kAttack), SubmitOutcome::kNotPlaying);
}

TEST(BattleRoomTest, StrangerCannotSubmitInput) {
    BattleRoom room = PlayingRoom();
    EXPECT_EQ(room.SubmitInput("p-9999", InputKind::kAttack), SubmitOutcome::kNotInRoom);
}

TEST(BattleRoomTest, AttackDamagesOpponent) {
    BattleRoom room = PlayingRoom();
    EXPECT_EQ(room.SubmitInput("p-0001", InputKind::kAttack), SubmitOutcome::kAccepted);

    // 推进一帧。
    room.Tick(kT0 + kFrameIntervalMs);

    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(snapshot.frame, 1);
    EXPECT_EQ(HpOf(snapshot, "p-0001"), kInitialHp);
    EXPECT_EQ(HpOf(snapshot, "p-0002"), kInitialHp - kAttackDamage);
}

TEST(BattleRoomTest, BothAttacksInSameFrameDamageBoth) {
    BattleRoom room = PlayingRoom();
    room.SubmitInput("p-0001", InputKind::kAttack);
    room.SubmitInput("p-0002", InputKind::kAttack);

    room.Tick(kT0 + kFrameIntervalMs);

    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(HpOf(snapshot, "p-0001"), kInitialHp - kAttackDamage);
    EXPECT_EQ(HpOf(snapshot, "p-0002"), kInitialHp - kAttackDamage);
}

TEST(BattleRoomTest, RepeatedInputInOneFrameCountsOnce) {
    BattleRoom room = PlayingRoom();
    // 同一帧内连点 5 次。
    for (int i = 0; i < 5; ++i) {
        room.SubmitInput("p-0001", InputKind::kAttack);
    }
    room.Tick(kT0 + kFrameIntervalMs);

    // 伤害按帧结算：允许同帧多次会变成「谁点得快谁赢」，那是延迟决定胜负。
    EXPECT_EQ(HpOf(room.Snapshot(), "p-0002"), kInitialHp - kAttackDamage);
}

TEST(BattleRoomTest, InputDoesNotCarryOverToNextFrame) {
    BattleRoom room = PlayingRoom();
    room.SubmitInput("p-0001", InputKind::kAttack);
    room.Tick(kT0 + kFrameIntervalMs);
    // 第二帧没有新输入，血量不应继续下降。
    room.Tick(kT0 + 2 * kFrameIntervalMs);

    EXPECT_EQ(HpOf(room.Snapshot(), "p-0002"), kInitialHp - kAttackDamage);
}

TEST(BattleRoomTest, TickWithoutEnoughElapsedTimeDoesNothing) {
    BattleRoom room = PlayingRoom();
    // 不足一帧的时间不应推进帧号。
    EXPECT_FALSE(room.Tick(kT0 + kFrameIntervalMs - 1));
    EXPECT_EQ(room.Snapshot().frame, 0);
}

// ---------------------------------------------------------------------------
// 胜负判定
// ---------------------------------------------------------------------------

TEST(BattleRoomTest, HpZeroEndsTheGameAndPicksWinner) {
    BattleRoom room = PlayingRoom();

    // 打死 p-0002：需要 kInitialHp / kAttackDamage 次有效攻击。
    const int hits = kInitialHp / kAttackDamage;
    std::int64_t now = kT0;
    for (int i = 0; i < hits; ++i) {
        room.SubmitInput("p-0001", InputKind::kAttack);
        now += kFrameIntervalMs;
        room.Tick(now);
    }

    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(snapshot.phase, RoomPhase::kFinishing);
    EXPECT_EQ(snapshot.finish_reason, FinishReason::kHpZero);
    EXPECT_EQ(snapshot.winner_id, "p-0001");
    // HP 不会变成负数。
    EXPECT_EQ(HpOf(snapshot, "p-0002"), 0);
    EXPECT_EQ(snapshot.finished_at_ms, now);
}

TEST(BattleRoomTest, SimultaneousKnockoutIsADraw) {
    BattleRoom room = PlayingRoom();

    const int hits = kInitialHp / kAttackDamage;
    std::int64_t now = kT0;
    for (int i = 0; i < hits; ++i) {
        room.SubmitInput("p-0001", InputKind::kAttack);
        room.SubmitInput("p-0002", InputKind::kAttack);
        now += kFrameIntervalMs;
        room.Tick(now);
    }

    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(snapshot.phase, RoomPhase::kFinishing);
    EXPECT_EQ(snapshot.finish_reason, FinishReason::kHpZero);
    // 同帧互杀按平局处理：winner_id 为空，而不是随便选一个。
    EXPECT_TRUE(snapshot.winner_id.empty());
}

TEST(BattleRoomTest, TimeoutPicksHigherHp) {
    BattleRoom room = PlayingRoom();

    // 只让 p-0001 攻击一次，然后在剩余帧里空转，直到达到最大帧数。
    room.SubmitInput("p-0001", InputKind::kAttack);
    RunToTimeout(&room);

    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(snapshot.phase, RoomPhase::kFinishing);
    EXPECT_EQ(snapshot.finish_reason, FinishReason::kTimeout);
    EXPECT_EQ(snapshot.winner_id, "p-0001");
}

TEST(BattleRoomTest, EqualHpAtTimeoutIsADraw) {
    BattleRoom room = PlayingRoom();

    // 谁都不攻击，双方血量始终相同。
    RunToTimeout(&room);

    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(snapshot.phase, RoomPhase::kFinishing);
    EXPECT_EQ(snapshot.finish_reason, FinishReason::kTimeout);
    EXPECT_TRUE(snapshot.winner_id.empty());
}

TEST(BattleRoomTest, InputAfterFinishIsRejected) {
    BattleRoom room = PlayingRoom();
    RunToTimeout(&room);
    ASSERT_EQ(room.phase(), RoomPhase::kFinishing);

    EXPECT_EQ(room.SubmitInput("p-0001", InputKind::kAttack), SubmitOutcome::kAlreadyFinished);
    EXPECT_EQ(room.Join("p-0001", kTimeoutAtMs), JoinOutcome::kAlreadyFinished);
}

TEST(BattleRoomTest, ResultIsAvailableOnlyAfterFinish) {
    BattleRoom room = PlayingRoom();
    EXPECT_FALSE(room.Result().has_value());

    RunToTimeout(&room);
    const auto result = room.Result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->match_id, "m-1");
    EXPECT_EQ(result->room_id, "r-1");
    EXPECT_EQ(result->player_count, 2);
    EXPECT_EQ(result->started_at_ms, kT0);
}

// ---------------------------------------------------------------------------
// 结果落库状态
// ---------------------------------------------------------------------------

TEST(BattleRoomTest, PersistSuccessMovesToFinished) {
    BattleRoom room = PlayingRoom();
    RunToTimeout(&room);
    ASSERT_EQ(room.phase(), RoomPhase::kFinishing);

    room.MarkResultPersisted();
    EXPECT_EQ(room.phase(), RoomPhase::kFinished);
}

TEST(BattleRoomTest, PersistFailureKeepsRoomInFinishing) {
    BattleRoom room = PlayingRoom();
    RunToTimeout(&room);

    // 第一次写入应当被立刻允许（不白等一个重试间隔）。
    // 实现把 last_persist_attempt_ms_ 置 0，因此任何真实时间点都满足。
    EXPECT_TRUE(room.ShouldRetryPersist(kTimeoutAtMs));

    room.MarkResultFailed(kTimeoutAtMs);
    // 失败后停留在 FINISHING，等下一个重试间隔。
    EXPECT_EQ(room.phase(), RoomPhase::kFinishing);
    EXPECT_FALSE(room.ShouldRetryPersist(kTimeoutAtMs + 1));
    EXPECT_TRUE(room.ShouldRetryPersist(kTimeoutAtMs + 1000));
}

// ---------------------------------------------------------------------------
// 等待加入超时
// ---------------------------------------------------------------------------

TEST(BattleRoomTest, WaitingTimeoutAbortsTheRoom) {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    room.Join("p-0001", kT0);

    EXPECT_FALSE(room.Tick(kT0 + kWaitingTimeoutMs - 1));
    EXPECT_TRUE(room.Tick(kT0 + kWaitingTimeoutMs));

    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(snapshot.phase, RoomPhase::kAborted);
    // 异常终止的对局不产生结果——不能写一行 winner 为空的战绩假装它打完了。
    EXPECT_FALSE(room.Result().has_value());
}

TEST(BattleRoomTest, AbortedRoomIsNotSubmittedToPersistence) {
    BattleRoom room("r-1", "m-1", {"p-0001", "p-0002"}, kT0);
    room.Tick(kT0 + kWaitingTimeoutMs);
    ASSERT_EQ(room.phase(), RoomPhase::kAborted);
    EXPECT_FALSE(room.ShouldRetryPersist(kT0 + kWaitingTimeoutMs));
}

// ---------------------------------------------------------------------------
// 快照
// ---------------------------------------------------------------------------

TEST(BattleRoomTest, SnapshotHistoryIsBounded) {
    BattleRoom room = PlayingRoom();

    // 推进远超过环形缓冲容量的帧数，验证历史不会无限增长。
    // 每帧分批推进：单次 Tick 有 kMaxCatchUpFrames 上限。
    std::int64_t now = kT0;
    for (int i = 0; i < 40; ++i) {
        now += kFrameIntervalMs * 10;
        room.Tick(now);
    }

    EXPECT_LE(room.SnapshotHistorySize(), kMaxSnapshotHistory);
    EXPECT_GT(room.SnapshotHistorySize(), 0U);
}

TEST(BattleRoomTest, TickClampsCatchUpFrames) {
    BattleRoom room = PlayingRoom();

    // 一次跳过 100 帧：超过 kMaxCatchUpFrames 的部分应被丢弃，
    // 而不是把 CPU 拉满去补偿。
    room.Tick(kT0 + 100 * kFrameIntervalMs);
    const std::int64_t frame_after_first = room.Snapshot().frame;
    EXPECT_EQ(frame_after_first, rgbt::room::kMaxCatchUpFrames);

    // 基准时间已经前移，因此紧接着再 Tick 不会继续补上剩余的帧。
    EXPECT_FALSE(room.Tick(kT0 + 100 * kFrameIntervalMs));
}

// ---------------------------------------------------------------------------
// 断线重连与宽限期（TASK-016）
//
// 这一组覆盖的是"谁在线"如何影响对局：暂停、恢复、到期判负、双方都断线作废。
// 时间全部由用例注入，因此 30 秒的宽限期不需要真的等 30 秒。
// ---------------------------------------------------------------------------

using rgbt::room::kReconnectGraceMs;

TEST(BattleRoomTest, SetPresenceMarksPlayerOffline) {
    BattleRoom room = PlayingRoom();

    EXPECT_EQ(room.SetPresence("p-0001", false, kT0), PresenceOutcome::kOk);

    const RoomSnapshot snapshot = room.Snapshot();
    ASSERT_EQ(snapshot.players.size(), 2U);
    EXPECT_FALSE(snapshot.players[0].online);
    EXPECT_TRUE(snapshot.players[1].online);
    // 断线者仍然**在房间里**：connected 与 online 是两件事。
    EXPECT_TRUE(snapshot.players[0].connected);
    EXPECT_TRUE(room.IsWaitingForReconnect());
}

TEST(BattleRoomTest, PresenceRejectsStrangerAndFinishedRoom) {
    BattleRoom room = PlayingRoom();
    EXPECT_EQ(room.SetPresence("p-9999", false, kT0), PresenceOutcome::kNotAMember);
    EXPECT_EQ(room.SetPresence("", false, kT0), PresenceOutcome::kInvalidArgument);

    // 打完之后连接状态不再影响任何结果：明确报 ALREADY_FINISHED，
    // 而不是"成功"——后者会掩盖调用方的时序错误。
    RunToTimeout(&room);
    ASSERT_EQ(room.phase(), RoomPhase::kFinishing);
    EXPECT_EQ(room.SetPresence("p-0001", false, kT0 + 1000), PresenceOutcome::kAlreadyFinished);
}

TEST(BattleRoomTest, DisconnectPausesTheMatch) {
    BattleRoom room = PlayingRoom();
    const std::int64_t offline_at = kT0 + 5 * kFrameIntervalMs;
    AdvanceTo(&room, kT0, offline_at);
    const std::int64_t frame_before = room.Snapshot().frame;
    ASSERT_GT(frame_before, 0);

    ASSERT_EQ(room.SetPresence("p-0001", false, offline_at), PresenceOutcome::kOk);
    // 整个宽限期内帧号都不前进——这是"暂停推进"而不是"继续推进"。
    AdvanceTo(&room, offline_at, offline_at + 10 * kFrameIntervalMs);
    EXPECT_EQ(room.Snapshot().frame, frame_before);
}

TEST(BattleRoomTest, ReconnectResumesWithoutLosingProgress) {
    BattleRoom room = PlayingRoom();
    const std::int64_t offline_at = kT0 + 5 * kFrameIntervalMs;
    AdvanceTo(&room, kT0, offline_at);
    const RoomSnapshot before = room.Snapshot();

    ASSERT_EQ(room.SetPresence("p-0001", false, offline_at), PresenceOutcome::kOk);
    AdvanceTo(&room, offline_at, offline_at + 10 * kFrameIntervalMs);

    // 宽限期内回来：帧号与血量都还是断线那一刻的值，因此"接着打"是精确的。
    ASSERT_EQ(room.SetPresence("p-0001", true, offline_at + 10 * kFrameIntervalMs),
              PresenceOutcome::kOk);
    const RoomSnapshot after = room.Snapshot();
    EXPECT_EQ(after.frame, before.frame);
    EXPECT_EQ(after.players[0].hp, before.players[0].hp);
    EXPECT_EQ(after.players[1].hp, before.players[1].hp);
    EXPECT_TRUE(after.players[0].online);
    EXPECT_FALSE(room.IsWaitingForReconnect());

    // 恢复后继续推进。
    room.Tick(offline_at + 11 * kFrameIntervalMs);
    EXPECT_GT(room.Snapshot().frame, before.frame);
}

TEST(BattleRoomTest, GraceExpiryMakesTheDisconnectedPlayerLose) {
    BattleRoom room = PlayingRoom();
    ASSERT_EQ(room.SetPresence("p-0002", false, kT0), PresenceOutcome::kOk);

    // 宽限期未到：不结束。
    room.Tick(kT0 + kReconnectGraceMs - kFrameIntervalMs);
    EXPECT_EQ(room.phase(), RoomPhase::kPlaying);

    // 宽限期到：判断线方负，且**产生胜负**（走 FINISHING 以落库）。
    room.Tick(kT0 + kReconnectGraceMs);
    EXPECT_EQ(room.phase(), RoomPhase::kFinishing);
    const RoomSnapshot snapshot = room.Snapshot();
    EXPECT_EQ(snapshot.finish_reason, FinishReason::kDisconnect);
    EXPECT_EQ(snapshot.winner_id, "p-0001");
    ASSERT_TRUE(room.Result().has_value());
    EXPECT_EQ(room.Result()->winner_id, "p-0001");
}

TEST(BattleRoomTest, BothOfflineExpiryAbortsWithoutDecidingAWinner) {
    BattleRoom room = PlayingRoom();
    ASSERT_EQ(room.SetPresence("p-0001", false, kT0), PresenceOutcome::kOk);
    ASSERT_EQ(room.SetPresence("p-0002", false, kT0), PresenceOutcome::kOk);

    room.Tick(kT0 + kReconnectGraceMs);
    EXPECT_EQ(room.phase(), RoomPhase::kAborted);
    EXPECT_EQ(room.Snapshot().finish_reason, FinishReason::kAborted);
    EXPECT_TRUE(room.Snapshot().winner_id.empty());
    // 作废的对局不产生结果，也就不会写进 match_results。
    EXPECT_FALSE(room.Result().has_value());
}

TEST(BattleRoomTest, BothOfflineWaitsForTheLastOneToExpire) {
    // 这条锁定的是"Gateway 重启"场景：所有订阅会同时断开，若按第一位到期就作废，
    // 任何一次 Gateway 重启都会立刻毁掉所有进行中的对局。因此要等最后一位也到期。
    BattleRoom room = PlayingRoom();
    const std::int64_t first_offline = kT0;
    const std::int64_t second_offline = kT0 + 20'000;
    ASSERT_EQ(room.SetPresence("p-0001", false, first_offline), PresenceOutcome::kOk);
    ASSERT_EQ(room.SetPresence("p-0002", false, second_offline), PresenceOutcome::kOk);

    // 第一位已到期，但第二位还在自己的宽限期内：不作废，继续等。
    room.Tick(first_offline + kReconnectGraceMs);
    EXPECT_EQ(room.phase(), RoomPhase::kPlaying);

    // 最后一位也到期：作废。
    room.Tick(second_offline + kReconnectGraceMs);
    EXPECT_EQ(room.phase(), RoomPhase::kAborted);
}

TEST(BattleRoomTest, RepeatedOfflineReportDoesNotResetTheGraceClock) {
    // 幂等：重复上报同一状态不能把宽限期无限推后（否则玩家可以靠刷请求赖着不走）。
    BattleRoom room = PlayingRoom();
    ASSERT_EQ(room.SetPresence("p-0001", false, kT0), PresenceOutcome::kOk);
    for (std::int64_t t = kT0 + 1000; t < kT0 + kReconnectGraceMs; t += 1000) {
        ASSERT_EQ(room.SetPresence("p-0001", false, t), PresenceOutcome::kOk);
    }

    room.Tick(kT0 + kReconnectGraceMs);
    EXPECT_EQ(room.phase(), RoomPhase::kFinishing);
    EXPECT_EQ(room.Snapshot().finish_reason, FinishReason::kDisconnect);
}

}  // namespace
