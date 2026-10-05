/// @file stream_hub_test.cpp
/// @brief StreamHub（SSE 订阅表与推送驱动）的单元测试。
///
/// 为什么这些用例值得单独写：整条推送链路里最容易出错、也最难在端到端脚本里
/// 观察到的恰恰是这几条：
///   * **没有订阅者时不能产生任何 Gateway→Room 轮询**（否则推送反而放大负载）；
///   * 同一房间多个订阅者只能扇入成一次轮询；
///   * 帧号没变时不能刷屏；
///   * 写失败必须清理订阅（SSE 下服务端收不到显式断开通知）；
///   * 对局结束后必须关闭流，否则长连接会把优雅退出卡住。
/// 端到端只能验证「能收到推送」，验证不了「没发生多余的事」。
///
/// 关于 SinkLog 为什么独立于 FakeSink：
///   订阅被清理时（对局结束、写失败、主动取消）StreamHub 会销毁 EventSink。
///   如果断言直接读 sink 的成员，那些用例就是 use-after-free——它在普通构建下
///   常常"看起来通过"，只在 ASan 或恰好被复用的内存上暴露。第一版测试就是这么
///   写的，跑出来两个 SegFault。因此把可观测状态放在 sink 之外的共享对象里。

#include "stream_hub.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "event_sink.hpp"
#include "room_client.hpp"

namespace {
using rgbt::gateway::EventSink;
using rgbt::gateway::RoomCallStatus;
using rgbt::gateway::RoomClient;
using rgbt::gateway::RoomPlayerSnapshot;
using rgbt::gateway::RoomSnapshot;
using rgbt::gateway::RoomState;
using rgbt::gateway::StreamHub;
using rgbt::gateway::StreamHubOptions;

constexpr std::int64_t kT0 = 1'000'000;

/// 与 `SubscribeOptions::heartbeat_interval_ms` 的默认值一致。
constexpr std::int64_t kHeartbeatIntervalMs = 15 * 1000;

/// 记录某个出口收到过什么。生命周期独立于 EventSink。
struct SinkLog {
    std::vector<std::string> writes;
    bool closed = false;
};

/// 可控的事件出口。`fail` 用来模拟客户端断开——SSE 下服务端只能靠写失败感知。
class FakeSink final : public EventSink {
public:
    explicit FakeSink(std::shared_ptr<SinkLog> log) : log_(std::move(log)) {}

    [[nodiscard]] bool Write(const std::string& data) override {
        log_->writes.push_back(data);
        return !fail;
    }

    void Close() override { log_->closed = true; }

    bool fail = false;

private:
    std::shared_ptr<SinkLog> log_;
};

/// 可控的房间客户端。只实现 StreamHub 用到的 GetState，其余方法给出固定结果。
class FakeRoomClient final : public RoomClient {
public:
    RoomCallStatus Join(const std::string&, const std::string&, const std::string&,
                        RoomSnapshot*) override {
        return RoomCallStatus::kOk;
    }
    RoomCallStatus SubmitAttack(const std::string&, const std::string&, const std::string&,
                                RoomSnapshot*) override {
        return RoomCallStatus::kOk;
    }
    RoomCallStatus GetState(const std::string& room_id, const std::string& /*request_id*/,
                            RoomSnapshot* out_snapshot) override {
        ++get_state_calls;
        const auto it = rooms.find(room_id);
        if (it == rooms.end()) {
            return RoomCallStatus::kNotFound;
        }
        *out_snapshot = it->second;
        return RoomCallStatus::kOk;
    }
    RoomCallStatus GetResult(const std::string&, const std::string&,
                             rgbt::gateway::MatchResultView*) override {
        return RoomCallStatus::kNotFound;
    }
    /// TASK-017：补发查询。返回预置的 SnapshotRange，方便逐个构造
    /// 「窗口内 / 窗口外 / id 超前」三种情况。
    RoomCallStatus GetSnapshotsSince(const std::string& room_id, std::int64_t since_frame,
                                     const std::string& /*request_id*/,
                                     rgbt::gateway::SnapshotRange* out_range) override {
        ++get_snapshots_calls;
        last_since_frame = since_frame;

        if (fail_snapshots) {
            return RoomCallStatus::kUnavailable;
        }
        if (ranges.find(room_id) == ranges.end()) {
            return RoomCallStatus::kNotFound;
        }
        *out_range = ranges[room_id];
        return RoomCallStatus::kOk;
    }
    /// TASK-016：记录每次连接状态上报，供用例断言
    /// 「订阅建立 → 上报 online；订阅移除 → 上报 offline」。
    RoomCallStatus SetPresence(const std::string& room_id, const std::string& player_id,
                               bool online, const std::string& /*request_id*/,
                               RoomSnapshot*) override {
        presence_calls.push_back(PresenceCall{room_id, player_id, online});
        if (fail_set_presence) {
            return RoomCallStatus::kUnavailable;
        }
        return RoomCallStatus::kOk;
    }
    [[nodiscard]] bool IsHealthy() override { return true; }

    struct PresenceCall {
        std::string room_id;
        std::string player_id;
        bool online = false;
    };

    std::unordered_map<std::string, RoomSnapshot> rooms;
    /// TASK-017：room_id -> 补发查询的返回结果。
    std::unordered_map<std::string, rgbt::gateway::SnapshotRange> ranges;
    int get_state_calls = 0;
    int get_snapshots_calls = 0;
    std::int64_t last_since_frame = -1;
    bool fail_snapshots = false;
    std::vector<PresenceCall> presence_calls;
    bool fail_set_presence = false;
};

RoomSnapshot PlayingRoom(const std::string& room_id, std::int64_t frame) {
    RoomSnapshot snapshot;
    snapshot.room_id = room_id;
    snapshot.match_id = "m-1";
    snapshot.state = RoomState::kPlaying;
    snapshot.frame = frame;
    snapshot.players.push_back(RoomPlayerSnapshot{"p-0001", 100, true});
    snapshot.players.push_back(RoomPlayerSnapshot{"p-0002", 100, true});
    return snapshot;
}

/// 注册订阅。返回的裸指针**只在订阅存活期间有效**，断言请使用 log。
struct Subscribed {
    std::uint64_t id = 0;
    FakeSink* sink = nullptr;
    std::shared_ptr<SinkLog> log;
};

Subscribed Subscribe(StreamHub* hub, const std::string& player_id, const std::string& room_id) {
    auto log = std::make_shared<SinkLog>();
    auto sink = std::make_unique<FakeSink>(log);
    Subscribed result;
    result.sink = sink.get();
    result.log = log;
    result.id = hub->Subscribe(player_id, room_id, std::move(sink)).id;
    return result;
}

/// 带 `Last-Event-ID` 的订阅（TASK-017）。返回值里的 id 可能为 0，
/// 用 `SubscribeReport` 查询补发结果。
Subscribed SubscribeSince(StreamHub* hub, const std::string& player_id, const std::string& room_id,
                          std::int64_t since_frame) {
    auto log = std::make_shared<SinkLog>();
    auto sink = std::make_unique<FakeSink>(log);
    Subscribed result;
    result.sink = sink.get();
    result.log = log;
    rgbt::gateway::SubscribeOptions options;
    options.last_event_id = since_frame;
    result.id = hub->Subscribe(player_id, room_id, std::move(sink), options).id;
    return result;
}

/// 带一个**非法**的 `Last-Event-ID` 订阅（请求头存在但解析失败）。
Subscribed SubscribeMalformed(StreamHub* hub, const std::string& player_id,
                              const std::string& room_id) {
    auto log = std::make_shared<SinkLog>();
    auto sink = std::make_unique<FakeSink>(log);
    Subscribed result;
    result.sink = sink.get();
    result.log = log;
    rgbt::gateway::SubscribeOptions options;
    options.last_event_id_malformed = true;
    result.id = hub->Subscribe(player_id, room_id, std::move(sink), options).id;
    return result;
}

/// 造一段补发结果：帧号 [from, to] 的连续快照，状态为 READY。
rgbt::gateway::SnapshotRange RangeOf(const std::string& room_id, std::int64_t from,
                                     std::int64_t to) {
    rgbt::gateway::SnapshotRange range;
    range.status = rgbt::gateway::SnapshotWindowStatus::kReady;
    range.oldest_frame = from;
    range.latest_frame = to;
    for (std::int64_t frame = from; frame <= to; ++frame) {
        range.snapshots.push_back(PlayingRoom(room_id, frame));
    }
    return range;
}

/// 第 n 次写入（按下标）里包含的帧号序列。用于断言补发顺序。
std::vector<std::int64_t> SequenceOfWrites(const std::shared_ptr<SinkLog>& log) {
    std::vector<std::int64_t> frames;
    for (const std::string& write : log->writes) {
        const std::size_t at = write.find("\"sequence\":");
        if (at == std::string::npos) {
            continue;
        }
        frames.push_back(std::stoll(write.substr(at + 11)));
    }
    return frames;
}

/// 取出所有写入里的 SSE `id: <n>` 行。补发的每个事件都必须带 id。
std::vector<std::int64_t> IdsOfWrites(const std::shared_ptr<SinkLog>& log) {
    std::vector<std::int64_t> ids;
    for (const std::string& write : log->writes) {
        if (write.rfind("id: ", 0) != 0) {
            continue;
        }
        const std::size_t end = write.find('\n');
        ids.push_back(std::stoll(write.substr(4, end - 4)));
    }
    return ids;
}

bool Contains(const std::shared_ptr<SinkLog>& log, const std::string& needle) {
    for (const std::string& write : log->writes) {
        if (write.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST(StreamHubTest, SubscribeReportsPlayerOnline) {
    // TASK-016：订阅建立 = 这个玩家的推送连接通了，必须如实上报给 Room。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    ASSERT_GT(sub.id, 0U);

    ASSERT_EQ(room.presence_calls.size(), 1U);
    EXPECT_EQ(room.presence_calls[0].room_id, "r-1");
    EXPECT_EQ(room.presence_calls[0].player_id, "p-0001");
    EXPECT_TRUE(room.presence_calls[0].online);
}

TEST(StreamHubTest, UnsubscribeReportsPlayerOfflineExactlyOnce) {
    // 连接结束要上报"断线"，让 Room 开始宽限计时；重复取消不应重复上报。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    ASSERT_EQ(room.presence_calls.size(), 1U);

    hub.Unsubscribe(sub.id);
    ASSERT_EQ(room.presence_calls.size(), 2U);
    EXPECT_FALSE(room.presence_calls[1].online);
    EXPECT_EQ(room.presence_calls[1].player_id, "p-0001");

    hub.Unsubscribe(sub.id);  // 幂等
    EXPECT_EQ(room.presence_calls.size(), 2U);
}

TEST(StreamHubTest, WriteFailureReportsPlayerOffline) {
    // 客户端断开时服务端收不到显式通知，只能靠写失败感知——这条路径也必须上报。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    sub.sink->fail = true;

    hub.Tick(kT0);
    hub.Tick(kT0 + 100);
    EXPECT_EQ(hub.ConnectionCount(), 0U);

    ASSERT_GE(room.presence_calls.size(), 2U);
    EXPECT_FALSE(room.presence_calls.back().online);
    EXPECT_EQ(room.presence_calls.back().player_id, "p-0001");
}

TEST(StreamHubTest, CloseAllReportsOfflineForEveryPlayer) {
    // 优雅退出时所有连接事实上都断了，必须逐个如实上报，否则这些对局
    // 在 Room 侧永远不会进入宽限期。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    room.rooms["r-2"] = PlayingRoom("r-2", 1);
    StreamHub hub(&room);

    Subscribe(&hub, "p-0001", "r-1");
    Subscribe(&hub, "p-0002", "r-2");
    ASSERT_EQ(room.presence_calls.size(), 2U);

    hub.CloseAll();
    ASSERT_EQ(room.presence_calls.size(), 4U);
    EXPECT_FALSE(room.presence_calls[2].online);
    EXPECT_FALSE(room.presence_calls[3].online);
}

TEST(StreamHubTest, OfflineStateChangeIsPushedEvenWhenFrameIsUnchanged) {
    // TASK-016 的关键一条：宽限期内对局**暂停推进**，帧号不变，但"对方断线了"
    // 恰恰是这一刻最该让客户端知道的事。若推送判据只看帧号，客户端在整个
    // 宽限期里收不到任何事件——所以判据必须是完整的状态签名。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);
    ASSERT_TRUE(Contains(sub.log, "room.state"));
    const std::size_t writes_after_first = sub.log->writes.size();

    // 帧号不变，只有 online 变成 false。
    room.rooms["r-1"].players[1].online = false;
    hub.Tick(kT0 + 100);

    EXPECT_GT(sub.log->writes.size(), writes_after_first);
    EXPECT_NE(sub.log->writes.back().find("\"online\":false"), std::string::npos);
}

// ---------------------------------------------------------------------------
// TASK-026：排空
// ---------------------------------------------------------------------------

TEST(StreamHubTest, BeginShutdownRejectsNewSubscriptions) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    hub.BeginShutdown();

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    EXPECT_EQ(sub.id, 0U) << "排空后不应再登记订阅（调用方据此给 503 shutting_down）";
    EXPECT_EQ(hub.ConnectionCount(), 0U);
}

TEST(StreamHubTest, CloseAllWithEventTellsTheClientWhy) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    ASSERT_NE(sub.id, 0U);

    const std::size_t closed = hub.CloseAllWithEvent("server_shutdown");

    EXPECT_EQ(closed, 1U);
    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_TRUE(sub.log->closed);
    // 客户端必须能看见**原因**：直接断开会让它把"服务优雅退出"当成"网络故障"，
    // 而这两者对应完全相反的下一步（停止重连 vs 立刻重连）。
    EXPECT_TRUE(Contains(sub.log, "stream.closed"));
    EXPECT_TRUE(Contains(sub.log, "server_shutdown"));
}

TEST(StreamHubTest, CloseAllWithEventOnEmptyHubIsSafe) {
    FakeRoomClient room;
    StreamHub hub(&room);
    EXPECT_EQ(hub.CloseAllWithEvent("server_shutdown"), 0U);
}

}  // namespace

TEST(StreamHubTest, NoSubscribersMeansNoRoomPolling) {
    // 这是 TASK-009 最重要的一条不变量：推送不能变成「无论如何都在轮询」。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    for (int i = 0; i < 5; ++i) {
        hub.Tick(kT0 + 100 * i);
    }

    EXPECT_EQ(hub.RoomPollCount(), 0U);
    EXPECT_EQ(room.get_state_calls, 0);
}

TEST(StreamHubTest, FirstTickSendsSessionReady) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);

    // session.ready 带上订阅者与房间，客户端据此确认订阅生效。
    ASSERT_TRUE(Contains(sub.log, "event: session.ready"));
    EXPECT_TRUE(Contains(sub.log, "\"player_id\":\"p-0001\""));
    EXPECT_TRUE(Contains(sub.log, "\"room_id\":\"r-1\""));
}

TEST(StreamHubTest, FrameChangePushesRoomState) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);
    const std::size_t after_ready = sub.log->writes.size();

    room.rooms["r-1"] = PlayingRoom("r-1", 2);
    hub.Tick(kT0 + 100);

    ASSERT_GT(sub.log->writes.size(), after_ready);
    EXPECT_TRUE(Contains(sub.log, "event: room.state"));
    EXPECT_TRUE(Contains(sub.log, "\"sequence\":2"));
}

TEST(StreamHubTest, UnchangedFrameIsNotPushed) {
    // 帧号没变就不推送。否则每 100 ms 都会刷一条完全相同的事件。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 7);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);
    const std::size_t baseline = sub.log->writes.size();

    for (int i = 1; i <= 5; ++i) {
        hub.Tick(kT0 + 100 * i);
    }

    EXPECT_EQ(sub.log->writes.size(), baseline);
    // 但轮询确实发生了：不推送不等于不检查。
    EXPECT_EQ(hub.RoomPollCount(), 6U);
}

TEST(StreamHubTest, MultipleSubscribersShareOnePoll) {
    // 扇入：同一房间 N 个订阅者，每轮只查一次 Room。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed first = Subscribe(&hub, "p-0001", "r-1");
    const Subscribed second = Subscribe(&hub, "p-0002", "r-1");
    hub.Tick(kT0);

    EXPECT_EQ(hub.RoomPollCount(), 1U);
    EXPECT_EQ(room.get_state_calls, 1);
    EXPECT_EQ(hub.ConnectionCount(), 2U);
    EXPECT_EQ(hub.SubscribedRoomCount(), 1U);
    // 两个订阅者都收到了同一份状态。
    EXPECT_TRUE(Contains(first.log, "event: room.state"));
    EXPECT_TRUE(Contains(second.log, "event: room.state"));
}

TEST(StreamHubTest, FinishedRoomPushesFinishedAndClosesStream) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 5);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);

    RoomSnapshot finished = PlayingRoom("r-1", 10);
    finished.state = RoomState::kFinished;
    finished.winner_id = "p-0001";
    finished.finish_reason = "hp_zero";
    room.rooms["r-1"] = finished;
    hub.Tick(kT0 + 100);

    EXPECT_TRUE(Contains(sub.log, "event: room.finished"));
    EXPECT_TRUE(Contains(sub.log, "\"winner_id\":\"p-0001\""));
    // 结束事件之后这条流没有内容了，必须被关闭并移除，否则连接会一直挂着。
    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_TRUE(sub.log->closed);
}

TEST(StreamHubTest, WriteFailureRemovesSubscription) {
    // 客户端断开时服务端收不到显式通知，只能靠写失败感知。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    sub.sink->fail = true;  // 此刻订阅还在，裸指针有效
    hub.Tick(kT0);

    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_TRUE(sub.log->closed);

    // 订阅被清理后，后续轮询也必须停下来。
    const std::uint64_t after = hub.RoomPollCount();
    hub.Tick(kT0 + 100);
    EXPECT_EQ(hub.RoomPollCount(), after);
}

TEST(StreamHubTest, HeartbeatIsSentWhileIdle) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHubOptions options;
    options.heartbeat_interval_ms = 1000;
    StreamHub hub(&room, options);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);

    EXPECT_FALSE(Contains(sub.log, ": ping"));
    hub.Tick(kT0 + 1000);
    EXPECT_TRUE(Contains(sub.log, ": ping"));
}

TEST(StreamHubTest, UnsubscribeIsIdempotentAndStopsPolling) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    ASSERT_GT(sub.id, 0U);

    hub.Unsubscribe(sub.id);
    hub.Unsubscribe(sub.id);  // 幂等：重复取消不应崩溃
    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_TRUE(sub.log->closed);

    hub.Tick(kT0);
    EXPECT_EQ(hub.RoomPollCount(), 0U);
}

TEST(StreamHubTest, RoomLookupFailureKeepsSubscriptionOpen) {
    // Room 抖动时不应关闭客户端的连接：恢复后同一房间还要继续推送。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    room.rooms.clear();

    hub.Tick(kT0);
    EXPECT_EQ(hub.ConnectionCount(), 1U);
    EXPECT_FALSE(sub.log->closed);

    // Room 恢复后继续推送。
    room.rooms["r-1"] = PlayingRoom("r-1", 9);
    hub.Tick(kT0 + 100);
    EXPECT_TRUE(Contains(sub.log, "\"sequence\":9"));
}

TEST(StreamHubTest, CloseAllClosesEveryStream) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    room.rooms["r-2"] = PlayingRoom("r-2", 1);
    StreamHub hub(&room);

    const Subscribed first = Subscribe(&hub, "p-0001", "r-1");
    const Subscribed second = Subscribe(&hub, "p-0002", "r-2");

    hub.CloseAll();

    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_TRUE(first.log->closed);
    EXPECT_TRUE(second.log->closed);
}

TEST(StreamHubTest, NullRoomClientDoesNotCrash) {
    // 没有房间客户端时订阅仍可建立（真实部署里不会走到，但接口允许 nullptr）。
    StreamHub hub(nullptr);
    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");

    hub.Tick(kT0);

    EXPECT_EQ(hub.ConnectionCount(), 1U);
    EXPECT_TRUE(Contains(sub.log, "event: session.ready"));
}

TEST(StreamHubTest, AbortedRoomAlsoEndsTheStream) {
    // ABORTED（等待加入超时）同样是对局终点：不写结果，但流必须结束。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 1);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);

    RoomSnapshot aborted = PlayingRoom("r-1", 3);
    aborted.state = RoomState::kAborted;
    aborted.finish_reason = "waiting_timeout";
    room.rooms["r-1"] = aborted;
    hub.Tick(kT0 + 100);

    EXPECT_TRUE(Contains(sub.log, "event: room.finished"));
    EXPECT_TRUE(Contains(sub.log, "\"state\":\"aborted\""));
    EXPECT_EQ(hub.ConnectionCount(), 0U);
}

// ---------------------------------------------------------------------------
// 推送连续性与补发（TASK-017）
//
// 这一组覆盖的是"重连时缺的帧怎么办"：补发顺序、去重、id 字段、窗口外与
// 非法 id 的处置。它们全都**无法靠端到端脚本观察**——端到端只能看到
// "重连之后帧号最终对上了"，看不出中间有没有重复帧、有没有该发 reset 却静默跳过。
// ---------------------------------------------------------------------------

TEST(StreamHubTest, FreshSubscriptionWithoutLastEventIdGetsNormalStatePush) {
    // 没有 Last-Event-ID = 第一次订阅。按任务单的失败场景：**不补发、也不发
    // stream.reset**，由第一次 Tick 正常推当前状态（等同 TASK-016 的行为）。
    // 这与"头存在但非法"（下面那条用例）刻意不同：那是客户端的 bug，必须被指出。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 7);
    StreamHub hub(&room);

    const Subscribed sub = Subscribe(&hub, "p-0001", "r-1");
    hub.Tick(kT0);

    EXPECT_FALSE(Contains(sub.log, "event: stream.reset"));
    EXPECT_EQ(hub.ResetEventCount("no_last_event_id"), 0U);
    // 状态照常送达，客户端第一帧就有画面。
    EXPECT_TRUE(Contains(sub.log, "event: room.state"));
    EXPECT_TRUE(Contains(sub.log, "\"sequence\":7"));
    // 新路径下不需要为"头缺失"去查历史区间：没有帧号，问了也没有意义。
    EXPECT_EQ(room.get_snapshots_calls, 0);
    EXPECT_EQ(hub.BackfilledFrameCount(), 0U);
    EXPECT_TRUE(Contains(sub.log, "event: session.ready"));
}

TEST(StreamHubTest, BackfillsMissingFramesInOrderWithIds) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 5);
    room.ranges["r-1"] = RangeOf("r-1", 3, 5);  // 客户端停在第 2 帧
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 2);
    hub.Tick(kT0);

    // 帧号递增、不重复。
    const std::vector<std::int64_t> frames = SequenceOfWrites(sub.log);
    ASSERT_EQ(frames.size(), 3U);
    EXPECT_EQ(frames[0], 3);
    EXPECT_EQ(frames[1], 4);
    EXPECT_EQ(frames[2], 5);

    // 每个补发的事件都带 SSE 的 id（客户端据此判断连续性）。
    const std::vector<std::int64_t> ids = IdsOfWrites(sub.log);
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(ids[0], 3);
    EXPECT_EQ(ids[1], 4);
    EXPECT_EQ(ids[2], 5);

    EXPECT_EQ(room.last_since_frame, 2);
    EXPECT_EQ(hub.BackfilledFrameCount(), 3U);

    const rgbt::gateway::SubscriptionReport report = hub.SubscribeReport(sub.id);
    EXPECT_TRUE(report.backfill_done);
    EXPECT_EQ(report.backfilled_frames, 3U);
    EXPECT_FALSE(report.reset_sent);
}

TEST(StreamHubTest, BackfillDoesNotDuplicateTheLatestState) {
    // 补发的最后一帧会把自己登记为"已推送状态"，因此同一轮/下一轮的实时推送
    // 不会再把同一份状态写一遍。否则客户端会看到重复帧号，
    // 而这恰恰是"补发"最容易引入的缺陷。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 5);
    room.ranges["r-1"] = RangeOf("r-1", 3, 5);
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 2);
    hub.Tick(kT0);
    const std::size_t after_backfill = sub.log->writes.size();

    hub.Tick(kT0 + 100);
    // 房间状态没变：不能有任何新的 room.state。
    for (std::size_t i = after_backfill; i < sub.log->writes.size(); ++i) {
        EXPECT_EQ(sub.log->writes[i].find("event: room.state"), std::string::npos);
    }

    // 房间推进后照常推送新帧。
    room.rooms["r-1"] = PlayingRoom("r-1", 6);
    hub.Tick(kT0 + 200);
    EXPECT_TRUE(Contains(sub.log, "\"sequence\":6"));
}

TEST(StreamHubTest, OutOfWindowBackfillSendsResetInsteadOfPartialHistory) {
    // 窗口外：缺的那段已经不在内存缓冲里了。必须明确告知"需要全量刷新"，
    // **不能**把半段历史发出去假装补上了——客户端会以为自己连续。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 9);
    rgbt::gateway::SnapshotRange stale;
    stale.status = rgbt::gateway::SnapshotWindowStatus::kIncomplete;
    stale.oldest_frame = 5;
    stale.latest_frame = 9;
    stale.snapshots = RangeOf("r-1", 7, 9).snapshots;
    room.ranges["r-1"] = stale;
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 1);
    hub.Tick(kT0);

    EXPECT_TRUE(Contains(sub.log, "event: stream.reset"));
    EXPECT_TRUE(Contains(sub.log, "\"reason\":\"id_out_of_window\""));
    // 载荷里必须带当前完整状态，客户端不必再发一次查询。
    EXPECT_TRUE(Contains(sub.log, "\"sequence\":9"));
    // 残缺的历史一帧都不发：这一轮只写 reset，下一轮写 session.ready + 当前状态。
    // 判据用"没有 room.state"而不是写次数——写次数会把 session.ready 的时机
    // 绑死在断言里，而那属于 Tick 的调度细节，不是本用例要锁定的语义。
    for (const std::string& write : sub.log->writes) {
        EXPECT_EQ(write.find("event: room.state"), std::string::npos)
            << "窗口外不应补发任何残缺的中间帧：" << write;
    }
    EXPECT_EQ(hub.ResetEventCount("id_out_of_window"), 1U);
    EXPECT_EQ(hub.BackfilledFrameCount(), 0U);
}

TEST(StreamHubTest, ClientAheadOfServerIsReportedAsIdAhead) {
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 3);
    rgbt::gateway::SnapshotRange ahead;
    ahead.status = rgbt::gateway::SnapshotWindowStatus::kAhead;
    ahead.oldest_frame = 1;
    ahead.latest_frame = 3;
    room.ranges["r-1"] = ahead;
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 99);
    hub.Tick(kT0);

    EXPECT_TRUE(Contains(sub.log, "\"reason\":\"id_ahead\""));
    EXPECT_EQ(hub.ResetEventCount("id_ahead"), 1U);
}

TEST(StreamHubTest, MalformedLastEventIdSendsResetWithItsOwnReason) {
    // 请求头存在但解析失败：这是客户端的 bug，不是"第一次订阅"。
    // 两者要能被分开观察到，否则排障时无法判断是谁的问题。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 2);
    StreamHub hub(&room);

    const Subscribed sub = SubscribeMalformed(&hub, "p-0001", "r-1");
    hub.Tick(kT0);

    EXPECT_TRUE(Contains(sub.log, "\"reason\":\"id_malformed\""));
    EXPECT_EQ(hub.ResetEventCount("id_malformed"), 1U);
    EXPECT_EQ(room.get_snapshots_calls, 0);  // 连帧号都没有，不该去查历史
}

TEST(StreamHubTest, ClientAlreadyCurrentGetsResetWithIdCurrent) {
    // 窗口内但没有缺失帧（刚断开又立刻重连）。仍然明确回一条 reset：
    // 客户端的 Last-Event-ID 说明它"以为"自己有某一帧，服务端要给出确认。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 4);
    room.ranges["r-1"] = RangeOf("r-1", 4, 3);  // 空区间，状态 READY
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 4);
    hub.Tick(kT0);

    EXPECT_TRUE(Contains(sub.log, "\"reason\":\"id_current\""));
    EXPECT_EQ(hub.ResetEventCount("id_current"), 1U);
    EXPECT_EQ(hub.BackfilledFrameCount(), 0U);
}

TEST(StreamHubTest, BackfillFailureKeepsSubscriptionOpen) {
    // 补发过程中 Room 不可用：不补发、不发事件、**不关闭连接**。
    // 沿用 TASK-009 的取舍——一次抖动不该逼客户端重连（客户端重连还会触发
    // 一次新的宽限期上报，代价比等待大得多）。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 5);
    room.ranges["r-1"] = RangeOf("r-1", 3, 5);  // 客户端停在第 2 帧
    room.fail_snapshots = true;
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 2);
    hub.Tick(kT0);

    EXPECT_EQ(hub.ConnectionCount(), 1U);
    EXPECT_FALSE(sub.log->closed);
    EXPECT_FALSE(Contains(sub.log, "event: stream.reset"));
    // 没补发成：这一轮不该有 room.state（既没有补发的帧，实时推送也让位给补发）。
    EXPECT_FALSE(Contains(sub.log, "event: room.state"));
    // 补发请求**没有被丢掉**：记在订阅上等下一轮重试。
    const rgbt::gateway::SubscriptionReport held = hub.SubscribeReport(sub.id);
    EXPECT_FALSE(held.backfill_done);

    // Room 恢复后的下一轮：补发照常完成（这一轮仍然只做补发，不掺实时推送）。
    room.fail_snapshots = false;
    hub.Tick(kT0 + 100);

    EXPECT_TRUE(Contains(sub.log, "event: room.state"));
    EXPECT_EQ(hub.SubscribeReport(sub.id).backfill_done, true);

    // 三帧补齐，一帧不多不少（帧号 3/4/5，客户端停在第 2 帧）。
    EXPECT_EQ(hub.SubscribeReport(sub.id).backfilled_frames, 3U);
    // 补发的每一帧都带 SSE 的 id，客户端据此判断连续性。
    EXPECT_TRUE(Contains(sub.log, "id: 3\nevent: room.state"));
    EXPECT_TRUE(Contains(sub.log, "id: 4\nevent: room.state"));
    EXPECT_TRUE(Contains(sub.log, "id: 5\nevent: room.state"));

    // 补发完成后的下一轮发出 session.ready（它由阶段一收集、补发让位一轮，
    // 因此排在补发之后；这条顺序本身是契约的一部分）。
    hub.Tick(kT0 + 200);
    EXPECT_TRUE(Contains(sub.log, "event: session.ready"));

    // 状态没变：此后实时推送保持安静，补发不会把同一状态再写一遍。
    const std::size_t quiet_baseline = sub.log->writes.size();
    hub.Tick(kT0 + 300);
    EXPECT_EQ(sub.log->writes.size(), quiet_baseline);
}

TEST(StreamHubTest, PendingBackfillStillGetsHeartbeatSoDeadPeersAreDetected) {
    // TASK-029：补发一直做不成（房间状态取不到）时，这条订阅**仍然必须定期被写一次**。
    //
    // 为什么这条用例是"订阅泄漏"的锁：SSE 下服务端收不到显式的断开通知，只能靠
    // **写失败**发现对端已经走了。如果一条订阅长期停在补发 pending、而每个 Tick 又
    // 什么都不写，它就永远发现不了对端关闭——订阅连同它的 socket 一起永久留下。
    // 长稳实测的症状正是如此：30 分钟里订阅数 50 -> 160、Gateway fd 114 -> 254，
    // 客户端全部离开 60 秒后仍残留 150 条订阅。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 5);
    room.ranges["r-1"] = RangeOf("r-1", 3, 5);
    room.fail_snapshots = true;  // 补发一直失败 -> 一直 pending
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 2);
    hub.Tick(kT0);

    // 刚建立、还没到心跳时刻：这一轮确实一次写都没有，计数要如实反映出来。
    EXPECT_EQ(hub.ConnectionCount(), 1U);
    EXPECT_EQ(sub.log->writes.size(), 0U);
    EXPECT_EQ(hub.SkippedNoWriteSubscriptions(), 1U);

    // 到了心跳间隔：**必须写一次**。少了这一步，下面对端断连就永远发现不了。
    hub.Tick(kT0 + kHeartbeatIntervalMs);
    EXPECT_GT(sub.log->writes.size(), 0U);

    // 对端其实已经断了：下一次心跳写失败 -> 订阅被回收、socket 被释放。
    ASSERT_NE(sub.sink, nullptr);
    sub.sink->fail = true;
    hub.Tick(kT0 + 2 * kHeartbeatIntervalMs);
    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_TRUE(sub.log->closed);
    EXPECT_EQ(hub.SubscriptionsClosedByWriteFailure(), 1U);
}

TEST(StreamHubTest, BackfillWriteFailureRemovesSubscription) {
    // 补发时写失败 = 连接已断，与实时推送走同一条清理路径。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 5);
    room.ranges["r-1"] = RangeOf("r-1", 3, 5);
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 2);
    ASSERT_NE(sub.sink, nullptr);
    sub.sink->fail = true;
    hub.Tick(kT0);

    EXPECT_EQ(hub.ConnectionCount(), 0U);
    EXPECT_TRUE(sub.log->closed);
}

TEST(StreamHubTest, BackfilledFinishedRoomClosesTheStream) {
    // 补发区间里包含终态：客户端应当收到 room.finished，并且流被关闭——
    // 否则一条已经打完的房间会留下永远不结束的订阅。
    FakeRoomClient room;
    RoomSnapshot finished = PlayingRoom("r-1", 5);
    finished.state = RoomState::kFinished;
    finished.winner_id = "p-0001";
    finished.finish_reason = "hp_zero";
    room.rooms["r-1"] = finished;
    room.ranges["r-1"] = RangeOf("r-1", 4, 4);
    room.ranges["r-1"].snapshots.push_back(finished);
    StreamHub hub(&room);

    const Subscribed sub = SubscribeSince(&hub, "p-0001", "r-1", 3);
    hub.Tick(kT0);

    EXPECT_TRUE(Contains(sub.log, "event: room.finished"));
    EXPECT_TRUE(Contains(sub.log, "\"winner_id\":\"p-0001\""));

    // 下一轮：终态必须仍然被送达一次（last_pushed_state_ 里不留终态），
    // 然后关闭流。
    hub.Tick(kT0 + 100);
    EXPECT_TRUE(Contains(sub.log, "event: room.finished"));
    EXPECT_EQ(hub.ConnectionCount(), 0U);
}

TEST(StreamHubTest, BackfillRunsOnlyOncePerSubscription) {
    // 补发是"建立订阅"这一步的事，不能每轮都查一次历史——那会把一次重连
    // 变成持续的历史查询负载。
    FakeRoomClient room;
    room.rooms["r-1"] = PlayingRoom("r-1", 5);
    room.ranges["r-1"] = RangeOf("r-1", 4, 5);
    StreamHub hub(&room);

    SubscribeSince(&hub, "p-0001", "r-1", 3);
    hub.Tick(kT0);
    EXPECT_EQ(room.get_snapshots_calls, 1);

    for (int i = 1; i <= 5; ++i) {
        hub.Tick(kT0 + 100 * i);
    }
    EXPECT_EQ(room.get_snapshots_calls, 1);
}
