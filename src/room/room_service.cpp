#include "room_service.hpp"

#include <brpc/closure_guard.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/logging.hpp"

namespace rgbt::room {
namespace {

/// 默认时钟：Unix 毫秒。
///
/// 用 system_clock 而不是 steady_clock，因为 started_at / finished_at 要落库成
/// TIMESTAMP，必须是可与其他系统对齐的墙上时间。
std::int64_t SystemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 填充统一错误体。
void SetError(rgbt::room::v1::RoomError* error, rgbt::room::v1::RoomErrorCode code,
              const char* reason, std::string message, const std::string& request_id) {
    error->set_code(code);
    error->set_reason(reason);
    error->set_message(std::move(message));
    error->set_request_id(request_id);
}

/// 把内部状态转成 proto 枚举。
rgbt::room::v1::RoomState ToProtoState(RoomPhase phase) {
    switch (phase) {
        case RoomPhase::kCreated:
            return rgbt::room::v1::ROOM_STATE_CREATED;
        case RoomPhase::kWaiting:
            return rgbt::room::v1::ROOM_STATE_WAITING;
        case RoomPhase::kPlaying:
            return rgbt::room::v1::ROOM_STATE_PLAYING;
        case RoomPhase::kFinishing:
            return rgbt::room::v1::ROOM_STATE_FINISHING;
        case RoomPhase::kFinished:
            return rgbt::room::v1::ROOM_STATE_FINISHED;
        case RoomPhase::kAborted:
            return rgbt::room::v1::ROOM_STATE_ABORTED;
    }
    return rgbt::room::v1::ROOM_STATE_UNSPECIFIED;
}

rgbt::room::v1::FinishReason ToProtoFinishReason(FinishReason reason) {
    switch (reason) {
        case FinishReason::kHpZero:
            return rgbt::room::v1::FINISH_REASON_HP_ZERO;
        case FinishReason::kTimeout:
            return rgbt::room::v1::FINISH_REASON_TIMEOUT;
        case FinishReason::kAborted:
            return rgbt::room::v1::FINISH_REASON_ABORTED;
        case FinishReason::kDisconnect:
            return rgbt::room::v1::FINISH_REASON_DISCONNECT;
        case FinishReason::kNone:
        default:
            return rgbt::room::v1::FINISH_REASON_UNSPECIFIED;
    }
}

/// 内部快照 -> proto 快照。
void FillSnapshot(const RoomSnapshot& source, rgbt::room::v1::RoomSnapshot* target) {
    target->set_room_id(source.room_id);
    target->set_match_id(source.match_id);
    target->set_state(ToProtoState(source.phase));
    target->set_frame(source.frame);
    target->set_finish_reason(ToProtoFinishReason(source.finish_reason));
    target->set_winner_id(source.winner_id);
    target->set_started_at_ms(source.started_at_ms);
    target->set_finished_at_ms(source.finished_at_ms);

    for (const PlayerSnapshot& player : source.players) {
        rgbt::room::v1::PlayerState* out = target->add_players();
        out->set_player_id(player.player_id);
        out->set_hp(player.hp);
        out->set_connected(player.connected);
        out->set_online(player.online);
    }
}

/// 内部结果 -> proto 结果。
void FillResult(const MatchResultRecord& source, rgbt::room::v1::MatchResult* target) {
    target->set_match_id(source.match_id);
    target->set_room_id(source.room_id);
    target->set_winner_id(source.winner_id);
    target->set_player_count(source.player_count);
    target->set_started_at_ms(source.started_at_ms);
    target->set_finished_at_ms(source.finished_at_ms);
}

/// 内部输入类型 -> proto 输入类型。
std::optional<InputKind> ToInputKind(rgbt::room::v1::PlayerInput::Kind kind) {
    switch (kind) {
        case rgbt::room::v1::PlayerInput::KIND_ATTACK:
            return InputKind::kAttack;
        case rgbt::room::v1::PlayerInput::KIND_UNSPECIFIED:
        default:
            return std::nullopt;
    }
}

}  // namespace

RoomServiceImpl::RoomServiceImpl(RoomManager* manager, std::function<std::int64_t()> clock)
    : manager_(manager), clock_(clock ? std::move(clock) : SystemNowMs) {
    // TASK-019：房间数按阶段暴露成 gauge。**每个阶段都要登记**（即使当前为 0）：
    // 否则"这个阶段还没有房间"在面板上会表现为"这条曲线不存在"，看的人无法区分
    // "没有"与"查询失败"。
    //
    // 名称相同、标签不同的 gauge 各自是一条序列，因此 Gauge 必须支持标签
    // （见 metrics.hpp）。按名字去重会让六个阶段只导出第一条，而且不报任何错。
    for (const RoomPhase phase : kAllRoomPhases) {
        rgbt::common::Metrics().Gauge(
            rgbt::common::kMetricRooms, "Room 当前房间数，按生命周期阶段分组",
            [this, phase]() -> std::uint64_t {
                return manager_ == nullptr
                           ? 0
                           : static_cast<std::uint64_t>(manager_->PhaseCounts().At(phase));
            },
            {{"phase", rgbt::room::ToString(phase)}});
    }

    // 帧推进用"所有房间当前帧号之和"：它天然单调不减，且不需要在 BattleRoom::Tick
    // 的热路径上再插一次写入（理由见 room_manager.hpp 的 TotalFramesAdvanced 注释）。
    rgbt::common::Metrics().Gauge(
        rgbt::common::kMetricRoomFramesAdvancedTotal, "Room 累计推进的帧数（所有房间帧号之和）",
        [this]() -> std::uint64_t {
            return manager_ == nullptr
                       ? 0
                       : static_cast<std::uint64_t>(manager_->TotalFramesAdvanced());
        });
    // TASK-019：房间快照写入成功/失败。RoomManager 早已在计数
    // （`SnapshotWriteCount` / `SnapshotFailureCount`），这里只是把它暴露出来，
    // 不再维护第二份计数。
    rgbt::common::Metrics().Gauge(
        rgbt::common::kMetricSnapshotWriteTotal, "Room 快照写入次数，按结果分组",
        [this]() -> std::uint64_t {
            return manager_ == nullptr ? 0 : manager_->SnapshotWriteCount();
        },
        {{"outcome", "ok"}});
    rgbt::common::Metrics().Gauge(
        rgbt::common::kMetricSnapshotWriteTotal, "Room 快照写入次数，按结果分组",
        [this]() -> std::uint64_t {
            return manager_ == nullptr ? 0 : manager_->SnapshotFailureCount();
        },
        {{"outcome", "failed"}});
    metrics_ready_ = true;
}

std::int64_t RoomServiceImpl::NowMs() const {
    return clock_ ? clock_() : SystemNowMs();
}

void RoomServiceImpl::CreateRoom(google::protobuf::RpcController* /*controller*/,
                                 const rgbt::room::v1::CreateRoomRequest* request,
                                 rgbt::room::v1::CreateRoomResponse* response,
                                 google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    if (manager_ == nullptr) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                 "房间管理器未初始化", request->request_id());
        return;
    }

    std::vector<std::string> player_ids(request->player_ids().begin(), request->player_ids().end());

    std::string room_id;
    RoomSnapshot snapshot;
    const CreateOutcome outcome =
        manager_->Create(request->match_id(), player_ids, NowMs(), &room_id, &snapshot);

    switch (outcome) {
        case CreateOutcome::kOk:
            // TASK-018：房间创建是"匹配成功"落到 Room 的那一步，带上 Match 传来的
            // request_id，三个服务的日志才能用同一个键串起来。
            rgbt::common::LogInfo("room_created", request->request_id(),
                                  {{"room", room_id},
                                   {"match_id", request->match_id()},
                                   {"players", std::to_string(player_ids.size())}});
            response->set_room_id(room_id);
            FillSnapshot(snapshot, response->mutable_room());
            return;
        case CreateOutcome::kInvalidArgument:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_INVALID_ARGUMENT,
                     "invalid_argument", "match_id 或玩家列表不合法", request->request_id());
            return;
        case CreateOutcome::kInternal:
        default:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                     "房间号生成失败", request->request_id());
            return;
    }
}

void RoomServiceImpl::JoinRoom(google::protobuf::RpcController* /*controller*/,
                               const rgbt::room::v1::JoinRoomRequest* request,
                               rgbt::room::v1::JoinRoomResponse* response,
                               google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    if (manager_ == nullptr) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                 "房间管理器未初始化", request->request_id());
        return;
    }

    RoomSnapshot snapshot;
    const std::optional<JoinOutcome> outcome =
        manager_->Join(request->room_id(), request->player_id(), NowMs(), &snapshot);

    if (!outcome.has_value()) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_FOUND, "room_not_found",
                 "房间不存在或已回收", request->request_id());
        return;
    }

    switch (*outcome) {
        case JoinOutcome::kOk:
            rgbt::common::LogInfo("room_joined", request->request_id(),
                                  {{"room", request->room_id()}, {"player", request->player_id()}});
            FillSnapshot(snapshot, response->mutable_room());
            return;
        case JoinOutcome::kNotAMember:
            // 房间存在但不是给这个玩家的。用**独立错误码**而不是 INVALID_ARGUMENT：
            // 调用方只映射错误码，复用同一个码会让「不是本局成员」在跨进程后
            // 退化成「参数不合法」。也不用 NOT_FOUND——房间确实存在，谎称不存在
            // 会误导排障。
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_A_MEMBER, "not_a_member",
                     "该玩家不是这一局的成员", request->request_id());
            return;
        case JoinOutcome::kAlreadyFinished:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_ALREADY_FINISHED,
                     "room_already_finished", "对局已结束，不再接受加入", request->request_id());
            return;
        case JoinOutcome::kInvalidArgument:
        default:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_INVALID_ARGUMENT,
                     "invalid_argument", "player_id 不合法", request->request_id());
            return;
    }
}

void RoomServiceImpl::SubmitInput(google::protobuf::RpcController* /*controller*/,
                                  const rgbt::room::v1::SubmitInputRequest* request,
                                  rgbt::room::v1::SubmitInputResponse* response,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    if (manager_ == nullptr) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                 "房间管理器未初始化", request->request_id());
        return;
    }

    const std::optional<InputKind> kind = ToInputKind(request->input().kind());
    if (!kind.has_value()) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INVALID_ARGUMENT, "invalid_input",
                 "不支持的输入类型", request->request_id());
        return;
    }

    RoomSnapshot snapshot;
    const std::optional<SubmitOutcome> outcome =
        manager_->SubmitInput(request->room_id(), request->player_id(), *kind, &snapshot);

    if (!outcome.has_value()) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_FOUND, "room_not_found",
                 "房间不存在或已回收", request->request_id());
        return;
    }

    FillSnapshot(snapshot, response->mutable_room());
    switch (*outcome) {
        case SubmitOutcome::kAccepted:
            response->set_accepted(true);
            return;
        case SubmitOutcome::kNotPlaying:
            response->set_accepted(false);
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_PLAYING,
                     "room_not_playing", "对局尚未开始", request->request_id());
            return;
        case SubmitOutcome::kNotInRoom:
            response->set_accepted(false);
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_A_MEMBER, "not_a_member",
                     "该玩家不是这一局的成员", request->request_id());
            return;
        case SubmitOutcome::kAlreadyFinished:
            response->set_accepted(false);
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_ALREADY_FINISHED,
                     "room_already_finished", "对局已结束，不再接受输入", request->request_id());
            return;
        case SubmitOutcome::kInvalidArgument:
        default:
            response->set_accepted(false);
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_INVALID_ARGUMENT,
                     "invalid_argument", "输入不合法", request->request_id());
            return;
    }
}

void RoomServiceImpl::GetRoomSnapshotsSince(
    google::protobuf::RpcController* /*controller*/,
    const rgbt::room::v1::GetRoomSnapshotsSinceRequest* request,
    rgbt::room::v1::GetRoomSnapshotsSinceResponse* response, google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    if (manager_ == nullptr) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                 "房间管理器未初始化", request->request_id());
        return;
    }

    const SnapshotRange range =
        manager_->GetSnapshotsSince(request->room_id(), request->since_frame(), NowMs());

    response->set_oldest_frame(range.oldest_frame);
    response->set_latest_frame(range.latest_frame);

    if (range.outcome == SnapshotRangeOutcome::kNotFound) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_FOUND, "room_not_found",
                 "房间不存在或已回收", request->request_id());
        return;
    }

    // 三种"能查到房间"的情况都返回 200：补不齐不是调用失败，而是一个必须让调用方
    // 知道的事实。用错误码表达会诱导调用方去重试一个重试一万次也一样的结果。
    switch (range.outcome) {
        case SnapshotRangeOutcome::kOk:
            response->set_status(rgbt::room::v1::SNAPSHOTS_READY);
            break;
        case SnapshotRangeOutcome::kIncomplete:
            response->set_status(rgbt::room::v1::SNAPSHOTS_INCOMPLETE);
            break;
        case SnapshotRangeOutcome::kAhead:
            response->set_status(rgbt::room::v1::SNAPSHOTS_AHEAD);
            break;
        case SnapshotRangeOutcome::kNotFound:
        default:
            response->set_status(rgbt::room::v1::SNAPSHOT_WINDOW_STATUS_UNSPECIFIED);
            break;
    }

    for (const RoomSnapshot& snapshot : range.snapshots) {
        FillSnapshot(snapshot, response->add_snapshots());
    }
}

void RoomServiceImpl::SetPlayerPresence(google::protobuf::RpcController* /*controller*/,
                                        const rgbt::room::v1::SetPlayerPresenceRequest* request,
                                        rgbt::room::v1::SetPlayerPresenceResponse* response,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    if (manager_ == nullptr) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                 "房间管理器未初始化", request->request_id());
        return;
    }

    RoomSnapshot snapshot;
    const std::optional<PresenceOutcome> outcome = manager_->SetPresence(
        request->room_id(), request->player_id(), request->connected(), NowMs(), &snapshot);
    if (!outcome.has_value()) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_FOUND, "room_not_found",
                 "房间不存在或已回收", request->request_id());
        return;
    }

    switch (*outcome) {
        case PresenceOutcome::kOk:
            // TASK-018：presence 上报是"客户端的推送连接建立/断开"落到 Room 的那一步。
            // 带上 Gateway 订阅时的 request_id，于是"谁在什么时候连上/断开"这条链路
            // 在 Gateway 与 Room 两侧可用同一个键对上。
            rgbt::common::LogInfo("presence_reported", request->request_id(),
                                  {{"room", request->room_id()},
                                   {"player", request->player_id()},
                                   {"online", request->connected() ? "true" : "false"}});
            FillSnapshot(snapshot, response->mutable_room());
            return;
        case PresenceOutcome::kNotAMember:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_A_MEMBER, "not_a_member",
                     "该玩家不是这一局的成员", request->request_id());
            return;
        case PresenceOutcome::kAlreadyFinished:
            // 对局已结束：连接状态不再影响任何结果。用 ALREADY_FINISHED 而不是"成功"，
            // 否则调用方会以为上报产生了效果。
            FillSnapshot(snapshot, response->mutable_room());
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_ALREADY_FINISHED,
                     "room_already_finished", "对局已结束，连接状态不再影响结果",
                     request->request_id());
            return;
        case PresenceOutcome::kInvalidArgument:
        default:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_INVALID_ARGUMENT,
                     "invalid_input", "玩家标识不合法", request->request_id());
            return;
    }
}

void RoomServiceImpl::GetRoomState(google::protobuf::RpcController* /*controller*/,
                                   const rgbt::room::v1::GetRoomStateRequest* request,
                                   rgbt::room::v1::GetRoomStateResponse* response,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    if (manager_ == nullptr) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                 "房间管理器未初始化", request->request_id());
        return;
    }

    RoomSnapshot snapshot;
    if (!manager_->GetState(request->room_id(), NowMs(), &snapshot)) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_FOUND, "room_not_found",
                 "房间不存在或已回收", request->request_id());
        return;
    }
    FillSnapshot(snapshot, response->mutable_room());
}

void RoomServiceImpl::GetMatchResult(google::protobuf::RpcController* /*controller*/,
                                     const rgbt::room::v1::GetMatchResultRequest* request,
                                     rgbt::room::v1::GetMatchResultResponse* response,
                                     google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    if (manager_ == nullptr) {
        SetError(response->mutable_error(), rgbt::room::v1::ROOM_INTERNAL, "room_internal",
                 "房间管理器未初始化", request->request_id());
        return;
    }

    MatchResultRecord record;
    RoomSnapshot snapshot;
    const ResultOutcome outcome =
        manager_->GetResult(request->match_id(), NowMs(), &record, &snapshot);

    switch (outcome) {
        case ResultOutcome::kOk:
            FillResult(record, response->mutable_result());
            return;
        case ResultOutcome::kPending:
            // 明确区分「还没有结果」与「结果是平局」：这里只填房间快照，
            // result 留空，并给出可判定的 reason。
            FillSnapshot(snapshot, response->mutable_room());
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_RESULT_PENDING,
                     "result_pending", "对局已结束但结果尚未写入，请稍后重试",
                     request->request_id());
            return;
        case ResultOutcome::kUnavailable:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_STORE_UNAVAILABLE,
                     "result_store_unavailable", "对局结果存储暂时不可用", request->request_id());
            return;
        case ResultOutcome::kNotFound:
        default:
            SetError(response->mutable_error(), rgbt::room::v1::ROOM_NOT_FOUND, "result_not_found",
                     "没有这条对局结果", request->request_id());
            return;
    }
}

}  // namespace rgbt::room
