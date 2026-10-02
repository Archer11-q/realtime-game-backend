#include "stream_hub.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

namespace rgbt::gateway {
namespace {

/// SSE 的心跳。以注释行发送：EventSource 会忽略它，但它让连接在长时间没有
/// 帧变化时也不会被中间层当作空闲连接回收。
constexpr const char* kHeartbeat = ": ping\n\n";

/// 协议版本。与 docs/05-api-and-data.md 第 2 节的推送信封一致——
/// 该信封原为 WebSocket 设计，改用 SSE 后字段不变，只是承载方式换了。
constexpr int kEnvelopeVersion = 1;

std::int64_t SystemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// JSON 字符串转义。
///
/// 为什么要手写而不是引 JSON 库：本模块的载荷字段全部来自服务端（房间号、
/// 玩家号、固定枚举），没有用户输入，需要转义的只有引号与反斜杠这类边界情况。
/// 为此新增一个 JSON 依赖不划算。**但转义必须做**——少一次转义就能让一条
/// 包含引号的错误信息把整个事件流变成非法 JSON。
std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    // 控制字符必须转义，否则会破坏 SSE 的行结构。
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                    out += buffer;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

const char* RoomStateName(RoomState state) {
    switch (state) {
        case RoomState::kWaiting:
            return "waiting";
        case RoomState::kPlaying:
            return "playing";
        case RoomState::kFinishing:
            return "finishing";
        case RoomState::kFinished:
            return "finished";
        case RoomState::kAborted:
            return "aborted";
        case RoomState::kCreated:
        default:
            return "created";
    }
}

/// 组装 SSE 事件。
///
/// 格式：`event: <name>\ndata: <json>\n\n`。`data` 必须是不含换行的单行 JSON，
/// 否则每一行都要各自加 `data: ` 前缀。本文件生成的所有 JSON 都是单行的。
std::string SseEvent(const char* event, const std::string& data) {
    std::string out;
    out.reserve(data.size() + 32);
    out += "event: ";
    out += event;
    out += "\ndata: ";
    out += data;
    out += "\n\n";
    return out;
}

/// 版本化信封。字段与 docs/05-api-and-data.md 第 2 节一致。
///
/// 不包含 request_id：它只用于「需要响应的客户端请求」，而推送是服务端单向发出的。
std::string Envelope(const char* type, std::int64_t sequence, std::int64_t now_ms,
                     const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 128);
    out += "{\"version\":";
    out += std::to_string(kEnvelopeVersion);
    out += ",\"type\":\"";
    out += type;
    out += "\",\"sequence\":";
    out += std::to_string(sequence);
    out += ",\"timestamp_ms\":";
    out += std::to_string(now_ms);
    out += ",\"payload\":";
    out += payload;
    out += "}";
    return out;
}

std::string PlayersJson(const std::vector<RoomPlayerSnapshot>& players) {
    std::string out = "[";
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "{\"player_id\":\"";
        out += JsonEscape(players[i].player_id);
        out += "\",\"hp\":";
        out += std::to_string(players[i].hp);
        out += ",\"connected\":";
        out += players[i].connected ? "true" : "false";
        out += "}";
    }
    out += "]";
    return out;
}

std::string RoomPayload(const RoomSnapshot& snapshot) {
    std::string out = "{\"room_id\":\"";
    out += JsonEscape(snapshot.room_id);
    out += "\",\"match_id\":\"";
    out += JsonEscape(snapshot.match_id);
    out += "\",\"state\":\"";
    out += RoomStateName(snapshot.state);
    out += "\",\"frame\":";
    out += std::to_string(snapshot.frame);
    out += ",\"players\":";
    out += PlayersJson(snapshot.players);
    out += ",\"finish_reason\":\"";
    out += JsonEscape(snapshot.finish_reason);
    out += "\",\"winner_id\":\"";
    out += JsonEscape(snapshot.winner_id);
    out += "\",\"started_at_ms\":";
    out += std::to_string(snapshot.started_at_ms);
    out += ",\"finished_at_ms\":";
    out += std::to_string(snapshot.finished_at_ms);
    out += "}";
    return out;
}

/// 订阅建立后的第一个事件。带上服务端时间，便于客户端校准与排障。
std::string SessionReadyJson(const std::string& player_id, const std::string& room_id,
                             std::int64_t now_ms) {
    std::string payload = "{\"player_id\":\"";
    payload += JsonEscape(player_id);
    payload += "\",\"room_id\":\"";
    payload += JsonEscape(room_id);
    payload += "\",\"server_time_ms\":";
    payload += std::to_string(now_ms);
    payload += "}";
    return Envelope("session.ready", 0, now_ms, payload);
}

/// 房间状态的推送。
///
/// sequence 直接用帧号：它天然单调递增，客户端据此可以发现丢事件。
std::string RoomStateJson(const RoomSnapshot& snapshot, std::int64_t now_ms) {
    return Envelope("room.state", snapshot.frame, now_ms, RoomPayload(snapshot));
}

std::string RoomFinishedJson(const RoomSnapshot& snapshot, std::int64_t now_ms) {
    return Envelope("room.finished", snapshot.frame, now_ms, RoomPayload(snapshot));
}

}  // namespace

StreamHub::StreamHub(RoomClient* room, StreamHubOptions options) : room_(room), options_(options) {}

StreamHub::~StreamHub() {
    Stop();
}

std::uint64_t StreamHub::Subscribe(std::string player_id, std::string room_id,
                                   std::unique_ptr<EventSink> sink) {
    if (sink == nullptr) {
        return 0;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t id = next_id_++;
    Subscription subscription;
    subscription.id = id;
    subscription.player_id = std::move(player_id);
    subscription.room_id = std::move(room_id);
    subscription.sink = std::shared_ptr<EventSink>(std::move(sink));

    // 清掉这个房间的「已推送帧号」，让下一个 Tick 立刻把当前状态推给新订阅者。
    // 代价是同房间的既有订阅者会多收一条重复的状态——相比之下，
    // 「新订阅者要等到下一次帧变化才有画面」是更糟的体验。
    last_pushed_frame_.erase(subscription.room_id);

    subscriptions_.emplace(id, std::move(subscription));
    return id;
}

void StreamHub::Unsubscribe(std::uint64_t id) {
    std::shared_ptr<EventSink> sink;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = subscriptions_.find(id);
        if (it == subscriptions_.end()) {
            return;  // 幂等：已断开的连接可能被清理两次
        }
        sink = it->second.sink;
        subscriptions_.erase(it);
    }
    // 在锁外关闭：Close 可能触发写与刷出。
    if (sink != nullptr) {
        sink->Close();
    }
}

void StreamHub::Tick(std::int64_t now_ms) {
    /// session.ready 需要带上订阅者与房间，因此在锁内就把它们取出来。
    /// 锁外再回表查询是一次数据竞争——订阅可能已被移除。
    struct ReadyTarget {
        std::uint64_t id;
        std::shared_ptr<EventSink> sink;
        std::string player_id;
        std::string room_id;
    };

    // 阶段一：锁内取快照。按房间分组以便扇入，另行收集需要首发与心跳的订阅。
    std::unordered_map<std::string, std::vector<SinkRef>> by_room;
    std::vector<ReadyTarget> need_ready;
    std::vector<SinkRef> need_heartbeat;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : subscriptions_) {
            const Subscription& sub = entry.second;
            if (sub.sink == nullptr) {
                continue;
            }
            by_room[sub.room_id].push_back(SinkRef{sub.id, sub.sink});
            if (!sub.ready_sent) {
                need_ready.push_back(ReadyTarget{sub.id, sub.sink, sub.player_id, sub.room_id});
            } else if (now_ms - sub.last_heartbeat_ms >= options_.heartbeat_interval_ms) {
                need_heartbeat.push_back(SinkRef{sub.id, sub.sink});
            }
        }
    }

    std::vector<std::uint64_t> failed;

    // 阶段二 / 三：锁外做 I/O（Room 调用与 EventSink 写）。
    for (const auto& entry : by_room) {
        TickRoom(entry.first, entry.second, now_ms, &failed);
    }

    for (const ReadyTarget& target : need_ready) {
        if (!target.sink->Write(SseEvent(
                "session.ready", SessionReadyJson(target.player_id, target.room_id, now_ms)))) {
            failed.push_back(target.id);
        }
    }

    for (const SinkRef& target : need_heartbeat) {
        if (!target.sink->Write(kHeartbeat)) {
            failed.push_back(target.id);
        }
    }

    // 阶段四：回锁内提交状态，并清理写失败的订阅。
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const ReadyTarget& target : need_ready) {
        const auto it = subscriptions_.find(target.id);
        if (it != subscriptions_.end()) {
            it->second.ready_sent = true;
            it->second.last_heartbeat_ms = now_ms;
        }
    }
    for (const SinkRef& target : need_heartbeat) {
        const auto it = subscriptions_.find(target.id);
        if (it != subscriptions_.end()) {
            it->second.last_heartbeat_ms = now_ms;
        }
    }
    for (const std::uint64_t id : failed) {
        const auto it = subscriptions_.find(id);
        if (it != subscriptions_.end()) {
            // 连接已断：关闭出口并移除订阅。不依赖客户端发任何东西——
            // SSE 下服务端不会收到显式的断开通知，只能靠写失败感知。
            std::shared_ptr<EventSink> sink = it->second.sink;
            subscriptions_.erase(it);
            if (sink != nullptr) {
                sink->Close();
            }
        }
    }
}

void StreamHub::TickRoom(const std::string& room_id, const std::vector<SinkRef>& sinks,
                         std::int64_t now_ms, std::vector<std::uint64_t>* failed) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++room_poll_count_;
    }

    if (room_ == nullptr || sinks.empty()) {
        return;
    }

    RoomSnapshot snapshot;
    if (room_->GetState(room_id, &snapshot) != RoomCallStatus::kOk) {
        // 房间暂时查不到（已回收，或 Room 不可用）。这里**不关闭连接**：
        // Room 抖动恢复后同一房间仍可继续推送，而关闭连接会强迫客户端重连。
        // 也不推送 error，避免一次故障刷出一串事件。
        return;
    }

    const bool finished =
        snapshot.state == RoomState::kFinished || snapshot.state == RoomState::kAborted;

    bool changed = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = last_pushed_frame_.find(room_id);
        const std::int64_t previous = it == last_pushed_frame_.end() ? -1 : it->second;
        // 帧号变化才推送，避免每 100 ms 刷一条完全相同的事件。
        // 但「进入结束态」本身是必须送达的事件，即使帧号没变。
        changed = snapshot.frame != previous || finished;
        if (changed) {
            if (finished) {
                last_pushed_frame_.erase(room_id);
            } else {
                last_pushed_frame_[room_id] = snapshot.frame;
            }
        }
    }
    if (!changed) {
        return;
    }

    if (finished) {
        const std::string data = SseEvent("room.finished", RoomFinishedJson(snapshot, now_ms));
        for (const SinkRef& target : sinks) {
            // 结束事件之后这条流就没有内容了，因此无论写成功与否都关闭它。
            target.sink->Write(data);
            failed->push_back(target.id);
        }
        return;
    }

    const std::string data = SseEvent("room.state", RoomStateJson(snapshot, now_ms));
    for (const SinkRef& target : sinks) {
        if (!target.sink->Write(data)) {
            failed->push_back(target.id);
        }
    }
}

void StreamHub::Start() {
    if (thread_.joinable()) {
        return;
    }
    stopping_.store(false);
    const std::int64_t interval_ms =
        options_.poll_interval_ms > 0 ? options_.poll_interval_ms : 100;
    thread_ = std::thread([this, interval_ms]() {
        const auto interval = std::chrono::milliseconds(interval_ms);
        while (!stopping_.load()) {
            Tick(SystemNowMs());
            std::this_thread::sleep_for(interval);
        }
    });
}

void StreamHub::Stop() {
    stopping_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void StreamHub::CloseAll() {
    std::unordered_map<std::uint64_t, Subscription> taken;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        taken.swap(subscriptions_);
        last_pushed_frame_.clear();
    }
    // 在锁外关闭：Close 会触发响应的收尾。
    for (auto& entry : taken) {
        if (entry.second.sink != nullptr) {
            entry.second.sink->Close();
        }
    }
}

std::size_t StreamHub::ConnectionCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_.size();
}

std::size_t StreamHub::SubscribedRoomCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, bool> rooms;
    for (const auto& entry : subscriptions_) {
        rooms[entry.second.room_id] = true;
    }
    return rooms.size();
}

std::uint64_t StreamHub::RoomPollCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return room_poll_count_;
}

}  // namespace rgbt::gateway
