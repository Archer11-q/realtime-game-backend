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
    RoomCallStatus Join(const std::string&, const std::string&, RoomSnapshot*) override {
        return RoomCallStatus::kOk;
    }
    RoomCallStatus SubmitAttack(const std::string&, const std::string&, RoomSnapshot*) override {
        return RoomCallStatus::kOk;
    }
    RoomCallStatus GetState(const std::string& room_id, RoomSnapshot* out_snapshot) override {
        ++get_state_calls;
        const auto it = rooms.find(room_id);
        if (it == rooms.end()) {
            return RoomCallStatus::kNotFound;
        }
        *out_snapshot = it->second;
        return RoomCallStatus::kOk;
    }
    RoomCallStatus GetResult(const std::string&, rgbt::gateway::MatchResultView*) override {
        return RoomCallStatus::kNotFound;
    }
    [[nodiscard]] bool IsHealthy() override { return true; }

    std::unordered_map<std::string, RoomSnapshot> rooms;
    int get_state_calls = 0;
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
    result.id = hub->Subscribe(player_id, room_id, std::move(sink));
    return result;
}

bool Contains(const std::shared_ptr<SinkLog>& log, const std::string& needle) {
    for (const std::string& write : log->writes) {
        if (write.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
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
