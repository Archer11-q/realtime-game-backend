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

/// 由订阅的关联键派生"轮询用"的 request_id（TASK-018）。
///
/// 为什么需要派生而不是直接用订阅的 request_id：同一条订阅在生命周期内会发起
/// 很多次 `GetRoomState`（每 100 ms 一次）。若它们全都用建立订阅时的那个
/// request_id，日志里就分不清"这一次轮询"和"建立订阅"——而排障时恰恰需要区分。
/// 加 `:poll` 后缀既保留了可关联性（前缀相同、可 grep 到同一条链路），
/// 又标明了来源。订阅没有关联键时回落到 `room:<id>`。
std::string PollRequestId(const std::string& subscription_request_id, const std::string& room_id) {
    if (subscription_request_id.empty()) {
        return "room:" + room_id;
    }
    return subscription_request_id + ":poll";
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
/// 格式：`event: <name>\ndata: <json>\n\n`；**带 id 时**在最前面多一行
/// `id: <frame>`。`data` 必须是不含换行的单行 JSON，否则每一行都要各自加
/// `data: ` 前缀。本文件生成的所有 JSON 都是单行的。
///
/// 为什么 id 行是可选的（TASK-017）：`session.ready` 与心跳不由帧号标识，
/// 给它们填一个假的 id 会让客户端的 `Last-Event-ID` 指向一个不存在的帧。
/// 只给 `room.state` / `room.finished` / `stream.reset` 带 id。
std::string SseEvent(const char* event, const std::string& data,
                     std::optional<std::int64_t> id = std::nullopt) {
    std::string out;
    out.reserve(data.size() + 48);
    if (id.has_value()) {
        // SSE 规范的 id 字段是 UTF-8 文本，不含 NUL 与换行。这里只写十进制帧号。
        out += "id: ";
        out += std::to_string(*id);
        out += "\n";
    }
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
        // TASK-016：online 必须出现在推送里。宽限期内帧号不动，客户端唯一能
        // 知道"对方断线了/回来了"的途径就是它；前端据此显示"对方已断线，等待重连"。
        out += ",\"online\":";
        out += players[i].online ? "true" : "false";
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
/// SSE 的 `id:` 也用帧号（TASK-017），于是重连时客户端能把它原样回传成
/// `Last-Event-ID`，服务端据此补发缺的帧——这正是"补发"和"序列号"用同一个
/// 数字的原因：两个编号一旦分开，就得额外维护一张"序号↔帧号"的映射表。
std::string RoomStateJson(const RoomSnapshot& snapshot, std::int64_t now_ms) {
    return Envelope("room.state", snapshot.frame, now_ms, RoomPayload(snapshot));
}

std::string RoomFinishedJson(const RoomSnapshot& snapshot, std::int64_t now_ms) {
    return Envelope("room.finished", snapshot.frame, now_ms, RoomPayload(snapshot));
}

/// 需要全量刷新的明确告知（TASK-017）。
///
/// 载荷里带**当前完整状态**：客户端收到它之后不需要再发一次查询请求，
/// 也从"我以为我有全部状态"切换成"我拿到的是权威全量"。
///
/// 为什么不复用 `room.state` 加一个标记字段：`stream.reset` 是**不连续**的信号，
/// 客户端必须能在一眼之内分辨"这是一帧补充"和"这是要你重来"。混在同一个事件类型里，
/// 任何一处忘记检查标记的地方都会把 reset 当成普通帧。
std::string StreamResetJson(const char* reason, const RoomSnapshot& snapshot, std::int64_t now_ms) {
    std::string payload = "{\"reason\":\"";
    payload += JsonEscape(reason);
    payload += "\",\"room\":";
    payload += RoomPayload(snapshot);
    payload += "}";
    return Envelope("stream.reset", snapshot.frame, now_ms, payload);
}

}  // namespace

StreamHub::StreamHub(RoomClient* room, StreamHubOptions options) : room_(room), options_(options) {}

StreamHub::~StreamHub() {
    Stop();
}

SubscriptionReport StreamHub::Subscribe(std::string player_id, std::string room_id,
                                        std::unique_ptr<EventSink> sink, SubscribeOptions options) {
    SubscriptionReport report;
    if (sink == nullptr) {
        return report;
    }

    // 先各留一份副本：下面会把它们 move 进订阅记录，而上报必须在**锁外**做。
    const std::string report_room_id = room_id;
    const std::string report_player_id = player_id;

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t id = next_id_++;
        report.id = id;
        Subscription subscription;
        subscription.id = id;
        subscription.player_id = std::move(player_id);
        subscription.room_id = std::move(room_id);
        subscription.request_id = options.request_id;
        subscription.sink = std::shared_ptr<EventSink>(std::move(sink));
        // TASK-017：补发请求挂在这里，由下一次 Tick 在锁外执行。
        // **不在 Subscribe 里写数据**：此刻 brpc 的响应还没提交，
        // 写出去的内容与后续 Tick 的写入顺序无法保证，而补发必须严格先于实时推送。
        subscription.backfill_pending = true;
        subscription.backfill_from = options.last_event_id;
        subscription.backfill_malformed = options.last_event_id_malformed;

        // 清掉这个房间的「已推送状态」，让下一个 Tick 立刻把当前状态推给新订阅者。
        // 代价是同房间的既有订阅者会多收一条重复的状态——相比之下，
        // 「新订阅者要等到下一次状态变化才有画面」是更糟的体验。
        last_pushed_state_.erase(subscription.room_id);

        subscriptions_.emplace(id, std::move(subscription));
    }

    // TASK-016：把"这个玩家的推送连接建立了"如实上报给 Room。
    // 在锁外做（本项目一贯规则：持锁不做 I/O）。
    ReportPresence(report_room_id, report_player_id, true, options.request_id);
    return report;
}

void StreamHub::ReportPresence(const std::string& room_id, const std::string& player_id,
                               bool online, const std::string& request_id) {
    if (room_ == nullptr) {
        return;
    }
    const RoomCallStatus status =
        room_->SetPresence(room_id, player_id, online, request_id, nullptr);
    if (status != RoomCallStatus::kOk && status != RoomCallStatus::kAlreadyFinished) {
        // 不重试、不阻塞推送：Room 短暂不可用时，客户端的轮询兜底仍然可用；
        // 下一次连接变化（或重连）会重新上报。
        std::fprintf(stderr,
                     "[gateway] 上报连接状态失败（不重试）：room_id=%s player_id=%s online=%d\n",
                     room_id.c_str(), player_id.c_str(), online ? 1 : 0);
    }
}

SubscriptionReport StreamHub::SubscribeReport(std::uint64_t id) {
    SubscriptionReport report;
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end()) {
        return report;  // id == 0 表示订阅不存在。
    }
    report.id = id;
    report.reset_sent = it->second.reset_sent;
    report.reset_reason = it->second.reset_reason;
    report.backfilled_frames = it->second.backfilled_frames;
    report.backfill_done = it->second.backfill_done;
    return report;
}

void StreamHub::Unsubscribe(std::uint64_t id) {
    std::shared_ptr<EventSink> sink;
    std::string room_id;
    std::string player_id;
    std::string request_id;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = subscriptions_.find(id);
        if (it == subscriptions_.end()) {
            return;  // 幂等：已断开的连接可能被清理两次
        }
        sink = it->second.sink;
        room_id = it->second.room_id;
        player_id = it->second.player_id;
        request_id = it->second.request_id;
        subscriptions_.erase(it);
    }
    // TASK-016：连接已经结束，上报"断线"让 Room 进入宽限期。
    // 注意这也覆盖 Gateway 主动断开（优雅退出）的情况——那是事实。
    ReportPresence(room_id, player_id, false, request_id);
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

    // 阶段一：锁内取快照。按房间分组以便扇入，另行收集需要首发、补发与心跳的订阅。
    std::unordered_map<std::string, std::vector<SinkRef>> by_room;
    /// room_id -> 该房间任一订阅的关联键，供轮询日志使用（TASK-018）。
    std::unordered_map<std::string, std::string> room_request_ids;
    std::vector<ReadyTarget> need_ready;
    std::vector<SinkRef> need_heartbeat;
    std::vector<BackfillTarget> need_backfill;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : subscriptions_) {
            const Subscription& sub = entry.second;
            if (sub.sink == nullptr) {
                continue;
            }
            // TASK-017：需要补发的订阅这一轮只做补发，**不参与实时推送**。
            // 否则同一份状态会被写两遍：一次是补发的最后一帧，一次是补发之后
            // 紧随的实时状态。让实时推送从下一轮开始，"补发 → 实时"的分界才是清楚的。
            //
            // "头缺失"的订阅**不属于**这一类：它没有帧号可补，必须走下面的正常路径
            // （发 session.ready + 推当前状态）。这里多一道判据而不是让
            // BackfillSubscription 提前返回，是因为那样会连 session.ready 一起跳过——
            // 客户端拿不到 ready，也永远等不到当前状态（实测被 10 个既有用例抓到）。
            // 把"要不要补发"的判断放在收集阶段，缺席路径就与 TASK-016 完全一致。
            const bool should_backfill =
                sub.backfill_pending && (sub.backfill_from.has_value() || sub.backfill_malformed);
            if (should_backfill) {
                need_backfill.push_back(BackfillTarget{sub.id, sub.sink, sub.room_id,
                                                       sub.request_id, sub.backfill_from,
                                                       sub.backfill_malformed});
                continue;
            }
            by_room[sub.room_id].push_back(SinkRef{sub.id, sub.sink});
            room_request_ids[sub.room_id] = sub.request_id;
            if (!sub.ready_sent) {
                need_ready.push_back(ReadyTarget{sub.id, sub.sink, sub.player_id, sub.room_id});
            } else if (now_ms - sub.last_heartbeat_ms >= options_.heartbeat_interval_ms) {
                need_heartbeat.push_back(SinkRef{sub.id, sub.sink});
            }
        }
    }

    std::vector<std::uint64_t> failed;

    // 阶段二 / 三：锁外做 I/O（Room 调用与 EventSink 写）。
    //
    // 补发排在最前面：客户端重连后必须先拿到缺的帧，再收到新帧。
    // 补发自己负责把这一轮的状态登记进 last_pushed_state_，
    // 因此同一轮里实时推送不会再重复写一遍同样的状态。
    std::vector<std::pair<std::uint64_t, std::size_t>> backfilled;  // id, 帧数
    std::vector<std::uint64_t> backfill_pending_kept;               // 需要下一轮重试的订阅
    std::uint64_t backfilled_frames = 0;
    std::vector<std::pair<std::uint64_t, const char*>> reset_done;  // id, reason
    for (const BackfillTarget& target : need_backfill) {
        const BackfillOutcome outcome = BackfillSubscription(target);
        if (outcome.write_failed) {
            failed.push_back(target.id);
            continue;
        }
        if (outcome.attempted) {
            backfilled.emplace_back(target.id, outcome.frames);
            backfilled_frames += outcome.frames;
        } else {
            backfill_pending_kept.push_back(target.id);
        }
        if (outcome.reset_sent) {
            reset_done.emplace_back(target.id, outcome.reset_reason);
        }
    }

    for (const auto& entry : by_room) {
        const auto request_id_it = room_request_ids.find(entry.first);
        const std::string poll_request_id = PollRequestId(
            request_id_it == room_request_ids.end() ? std::string() : request_id_it->second,
            entry.first);
        TickRoom(entry.first, entry.second, poll_request_id, now_ms, &failed);
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
    std::vector<std::pair<std::string, std::string>> gone;  // room_id, player_id
    std::vector<std::string> gone_request_ids;              // 与 gone 一一对应
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : backfilled) {
            const auto it = subscriptions_.find(entry.first);
            if (it != subscriptions_.end()) {
                // 补发已完成：清掉请求，下一轮开始走实时推送。
                // last_heartbeat_ms 也一起重置，免得刚建好连接就立刻收到一条心跳。
                //
                // ready_sent **不在这里置位**：session.ready 的发送在下面同一轮的
                // 阶段三（need_ready 收集于阶段一），因此每条订阅恰好收到一次——
                // 补发的帧或 stream.reset 在它之前写出，顺序是确定的。
                it->second.backfill_pending = false;
                it->second.backfill_from.reset();
                it->second.backfill_done = true;
                it->second.backfilled_frames = entry.second;
                it->second.last_heartbeat_ms = now_ms;
                // 注意：`backfill_pending` 只在**补发真的执行过**之后清除
                // （BackfillSubscription 把 Room 不可用那种情况排除在 backfilled 之外）。
                // 否则 Room 一次抖动就会静默丢掉这次补发请求，客户端只能靠帧号
                // 跳变自己发现——而这正是本任务要消除的情况。
            }
        }
        backfilled_frame_count_ += backfilled_frames;
        for (const auto& entry : reset_done) {
            ++reset_counts_[entry.second];
            const auto it = subscriptions_.find(entry.first);
            if (it != subscriptions_.end()) {
                it->second.reset_sent = true;
                it->second.reset_reason = entry.second;
            }
        }
        // 没能处理的补发请求保持 pending：下一轮 Tick 重试（不清 backfill_from）。
        // 这里只重置心跳时间，避免刚建连接就收到一条心跳。
        for (const std::uint64_t id : backfill_pending_kept) {
            const auto it = subscriptions_.find(id);
            if (it != subscriptions_.end()) {
                it->second.last_heartbeat_ms = now_ms;
            }
        }
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
                //
                // TASK-016：这是**客户端断网**这一最常见路径，同样必须上报 offline
                // （由 stream_hub_test 的 WriteFailureReportsPlayerOffline 锁定）。
                // 上了锁，所以只把 id 收集起来，出锁后再做 I/O。
                gone.emplace_back(it->second.room_id, it->second.player_id);
                gone_request_ids.push_back(it->second.request_id);
                std::shared_ptr<EventSink> sink = it->second.sink;
                subscriptions_.erase(it);
                if (sink != nullptr) {
                    sink->Close();
                }
            }
        }
    }

    // 出锁后再上报：断网的客户端必须让 Room 进入宽限期，否则这些对局在服务端
    // 看起来一切正常，而玩家其实已经掉线了。
    for (std::size_t i = 0; i < gone.size(); ++i) {
        ReportPresence(gone[i].first, gone[i].second, false, gone_request_ids[i]);
    }
}

StreamHub::BackfillOutcome StreamHub::BackfillSubscription(const BackfillTarget& target) {
    BackfillOutcome outcome;
    if (room_ == nullptr) {
        return outcome;  // 没有房间客户端：无法补发，但连接本身是好的。
    }

    // 历史区间只查一次，并且只在真的有帧号时查（见下面的 malformed 分支）。
    SnapshotRange range;

    // 发一条 stream.reset 并收尾。四条分支唯一的区别就是 reason，
    // 因此把"写事件 + 登记状态 + 填返回值"放在一处——少写三遍就不会漏掉其中一步。
    //
    // `payload` 必须是**当前完整状态**：客户端要按它刷新，而不是再发一次查询。
    // 取不到它时不调用本函数（见各分支的 have_current 判断）。
    const auto send_reset = [&](const char* reason, const RoomSnapshot& payload) {
        std::fprintf(stderr,
                     "[gateway] 发出 stream.reset：room_id=%s reason=%s window=[%lld,%lld]\n",
                     target.room_id.c_str(), reason, static_cast<long long>(range.oldest_frame),
                     static_cast<long long>(range.latest_frame));
        outcome.write_failed = !target.sink->Write(SseEvent(
            "stream.reset", StreamResetJson(reason, payload, SystemNowMs()), payload.frame));
        outcome.reset_sent = !outcome.write_failed;
        outcome.reset_reason = reason;
        // 即使发出的是 reset，也要把它登记为本房间"已推送的状态"：
        // 否则同一轮/下一轮的实时推送会再写一遍完全相同的状态。
        SetLastPushedState(target.room_id, payload, /*terminal=*/false);
        return outcome;
    };

    if (target.malformed) {
        // 请求头存在但解析失败：这是客户端的 bug，不是"第一次订阅"。
        // 不静默当成首次订阅——那样客户端会以为自己拿到了连续的事件，
        // 而它其实连"从哪里开始"都没说清楚。
        //
        // **不查历史区间**：连帧号都没有，"从哪补"无从谈起。这同时是一条可断言的
        // 不变量（stream_hub_test 的 MalformedLastEventIdSendsResetWithItsOwnReason
        // 断言 get_snapshots_calls == 0）。
        RoomSnapshot current;
        if (room_->GetState(target.room_id, PollRequestId(target.request_id, target.room_id),
                            &current) != RoomCallStatus::kOk) {
            return outcome;  // 房间查不到：保持连接，什么都不发。
        }
        outcome.attempted = true;
        return send_reset("id_malformed", current);
    }

    const bool known_since_frame = target.since_frame.has_value();
    if (!known_since_frame) {
        // 请求头缺失 = 第一次订阅。**不补发、不发 `stream.reset`**，按今天的行为由
        // 下一次 Tick 正常推当前状态。项目所有者于 2026-10-02 确认按任务单的失败场景
        // 那一条实现（"缺失 → 不补发，直接按当前状态推，等同今天的行为，不报错"），
        // 而不是范围第 4 条里"缺失也发 reset"的读法。
        //
        // 这与 `id_malformed` 的处理**刻意不同**：头缺失是正常路径（新客户端本来
        // 就没有任何帧号），头存在却解析不出来是客户端实现有问题，不能混为一谈。
        //
        // 注意：走到这里本身就不应该发生——Tick 的阶段一已经按同样的判据把这种订阅
        // 排除在补发之外了。保留这个分支是为了让"补发只处理真有帧号或明确非法的情况"
        // 这条不变量在任何调用路径下都成立，而不是依赖调用方做对。
        return outcome;
    }
    const std::int64_t since_frame = *target.since_frame;
    if (room_->GetSnapshotsSince(target.room_id, since_frame,
                                 PollRequestId(target.request_id, target.room_id),
                                 &range) != RoomCallStatus::kOk) {
        // Room 不可用（或房间已回收）：**不补发、不发事件、不关闭连接**。
        // 沿用 TASK-009 的取舍——一次抖动的代价不该是逼客户端重连；
        // 下一轮 Tick 会照常推当前状态（它同样会失败，直到 Room 回来）。
        std::fprintf(stderr,
                     "[gateway] 补发失败（Room 不可用，连接保持）：room_id=%s since_frame=%lld\n",
                     target.room_id.c_str(), static_cast<long long>(since_frame));
        return outcome;
    }

    // 判断这条订阅要不要发 stream.reset。三种"要"的情况：窗口外、id 超前、无缺失帧。
    // （头缺失在更早的分支里已经返回，不在此列。）
    const bool need_reset = range.status != SnapshotWindowStatus::kReady || range.snapshots.empty();
    const char* reset_reason = "id_current";
    if (range.status == SnapshotWindowStatus::kAhead) {
        reset_reason = "id_ahead";
    } else if (range.status != SnapshotWindowStatus::kReady) {
        reset_reason = "id_out_of_window";
    } else if (range.snapshots.empty()) {
        // 窗口内但没有缺失帧：客户端已经和房间一样新（刚断开又立刻重连）。
        // 仍然发 reset：客户端的 Last-Event-ID 说明它"以为"自己有某一帧，
        // 服务端要给出一次明确确认，否则它只能靠超时猜测。
        reset_reason = "id_current";
    }

    // "当前完整状态"只为 reset 取：窗口内补发不需要它，而多一次 RPC 会让每一次
    // 重连都平白多一个可能超时的依赖调用。
    RoomSnapshot current;
    const bool have_current =
        need_reset &&
        room_->GetState(target.room_id, PollRequestId(target.request_id, target.room_id),
                        &current) == RoomCallStatus::kOk;
    if (need_reset) {
        if (!have_current) {
            // 取不到当前状态：不发一条指向未知状态的指令。保持连接，
            // 下一轮 Tick 会在 Room 恢复后照常推当前状态。
            return outcome;
        }
        return send_reset(reset_reason, current);
    }

    for (const RoomSnapshot& snapshot : range.snapshots) {
        // 补发的事件同样带 id：客户端据此判断是否连续，也能在再次断开后继续用它。
        // 事件类型按状态区分，与实时推送完全一致——客户端不需要为补发写第二条分支。
        const bool terminal =
            snapshot.state == RoomState::kFinished || snapshot.state == RoomState::kAborted;
        const std::string data =
            terminal
                ? SseEvent("room.finished", RoomFinishedJson(snapshot, SystemNowMs()),
                           snapshot.frame)
                : SseEvent("room.state", RoomStateJson(snapshot, SystemNowMs()), snapshot.frame);
        if (!target.sink->Write(data)) {
            outcome.write_failed = true;  // 连接已断：调用方清理订阅。
            return outcome;
        }
        SetLastPushedState(target.room_id, snapshot, terminal);
    }
    outcome.attempted = true;
    outcome.frames = range.snapshots.size();

    std::fprintf(stderr, "[gateway] 已补发 %zu 帧：room_id=%s since_frame=%lld\n",
                 range.snapshots.size(), target.room_id.c_str(),
                 static_cast<long long>(since_frame));
    return outcome;
}

void StreamHub::SetLastPushedState(const std::string& room_id, const RoomSnapshot& snapshot,
                                   bool terminal) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (terminal) {
        // 与 TickRoom 一致：结束态不留在表里，保证终局事件一定被送达一次。
        last_pushed_state_.erase(room_id);
        return;
    }
    last_pushed_state_[room_id] = StateSignature(snapshot);
}

std::string StreamHub::StateSignature(const RoomSnapshot& snapshot) {
    std::string signature = std::to_string(snapshot.frame);
    signature.push_back('|');
    signature += std::to_string(static_cast<int>(snapshot.state));
    for (const RoomPlayerSnapshot& player : snapshot.players) {
        signature.push_back('|');
        signature += player.player_id;
        signature.push_back(':');
        signature += std::to_string(player.hp);
        // online 必须进签名：宽限期内帧号不动，而变化恰恰只有它。
        signature.push_back(player.online ? '1' : '0');
    }
    return signature;
}

void StreamHub::TickRoom(const std::string& room_id, const std::vector<SinkRef>& sinks,
                         const std::string& request_id, std::int64_t now_ms,
                         std::vector<std::uint64_t>* failed) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++room_poll_count_;
    }

    if (room_ == nullptr || sinks.empty()) {
        return;
    }

    RoomSnapshot snapshot;
    if (room_->GetState(room_id, request_id, &snapshot) != RoomCallStatus::kOk) {
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
        const std::string current = StateSignature(snapshot);
        const auto it = last_pushed_state_.find(room_id);
        const std::string previous = it == last_pushed_state_.end() ? std::string() : it->second;
        // **状态没变才不推送**，而"状态"不只是帧号（TASK-016）：
        // 宽限期内对局暂停推进、帧号不变，但"对方断线了"恰恰是这一刻最该让客户端
        // 知道的事。只比帧号会让客户端在整个宽限期里收不到任何事件。
        // 但「进入结束态」本身是必须送达的事件，即使状态签名没变。
        changed = current != previous || finished;
        if (changed) {
            if (finished) {
                last_pushed_state_.erase(room_id);
            } else {
                last_pushed_state_[room_id] = current;
            }
        }
    }
    if (!changed) {
        return;
    }

    if (finished) {
        // TASK-017：终局事件同样带 id，客户端才能在结束前判断"我漏了帧"，
        // 并且在收到它之后把 Last-Event-ID 停在这一帧。
        const std::string data =
            SseEvent("room.finished", RoomFinishedJson(snapshot, now_ms), snapshot.frame);
        for (const SinkRef& target : sinks) {
            // 结束事件之后这条流就没有内容了，因此无论写成功与否都关闭它。
            target.sink->Write(data);
            failed->push_back(target.id);
        }
        return;
    }

    const std::string data =
        SseEvent("room.state", RoomStateJson(snapshot, now_ms), snapshot.frame);
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
        last_pushed_state_.clear();
    }
    // 在锁外关闭：Close 会触发响应的收尾；上报同样在锁外（不开 I/O 时持锁）。
    for (auto& entry : taken) {
        // TASK-016：进程要走了，这些连接事实上就是断了。如实上报，
        // 让 Room 开始各自的宽限计时——客户端重连后会重新上报 online。
        ReportPresence(entry.second.room_id, entry.second.player_id, false,
                       entry.second.request_id);
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

std::uint64_t StreamHub::BackfilledFrameCount() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return backfilled_frame_count_;
}

std::uint64_t StreamHub::ResetEventCount(const std::string& reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = reset_counts_.find(reason);
    return it == reset_counts_.end() ? 0 : it->second;
}

}  // namespace rgbt::gateway
