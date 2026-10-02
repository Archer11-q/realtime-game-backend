#include "brpc_room_client.hpp"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include <string>
#include <utility>

#include "room.pb.h"

namespace rgbt::gateway {
namespace {

/// 把 Room 的状态枚举转成 Gateway 自己的枚举。
RoomState ToState(rgbt::room::v1::RoomState state) {
    switch (state) {
        case rgbt::room::v1::ROOM_STATE_WAITING:
            return RoomState::kWaiting;
        case rgbt::room::v1::ROOM_STATE_PLAYING:
            return RoomState::kPlaying;
        case rgbt::room::v1::ROOM_STATE_FINISHING:
            return RoomState::kFinishing;
        case rgbt::room::v1::ROOM_STATE_FINISHED:
            return RoomState::kFinished;
        case rgbt::room::v1::ROOM_STATE_ABORTED:
            return RoomState::kAborted;
        case rgbt::room::v1::ROOM_STATE_CREATED:
        case rgbt::room::v1::ROOM_STATE_UNSPECIFIED:
        default:
            return RoomState::kCreated;
    }
}

/// 结束原因转成稳定的字符串。
///
/// 用字符串而不是枚举：浏览器直接可用，不必依赖枚举数字（与 MatchStatusInfo
/// 的 state 字段同一考虑）。
std::string ToFinishReasonString(rgbt::room::v1::FinishReason reason) {
    switch (reason) {
        case rgbt::room::v1::FINISH_REASON_HP_ZERO:
            return "hp_zero";
        case rgbt::room::v1::FINISH_REASON_TIMEOUT:
            return "timeout";
        case rgbt::room::v1::FINISH_REASON_ABORTED:
            return "aborted";
        case rgbt::room::v1::FINISH_REASON_UNSPECIFIED:
        default:
            return "none";
    }
}

/// 房间快照转换。
RoomSnapshot ToSnapshot(const rgbt::room::v1::RoomSnapshot& source) {
    RoomSnapshot snapshot;
    snapshot.room_id = source.room_id();
    snapshot.match_id = source.match_id();
    snapshot.state = ToState(source.state());
    snapshot.frame = source.frame();
    snapshot.finish_reason = ToFinishReasonString(source.finish_reason());
    snapshot.winner_id = source.winner_id();
    snapshot.started_at_ms = source.started_at_ms();
    snapshot.finished_at_ms = source.finished_at_ms();
    snapshot.players.reserve(static_cast<std::size_t>(source.players_size()));
    for (const rgbt::room::v1::PlayerState& player : source.players()) {
        RoomPlayerSnapshot out;
        out.player_id = player.player_id();
        out.hp = player.hp();
        out.connected = player.connected();
        snapshot.players.push_back(std::move(out));
    }
    return snapshot;
}

/// 把 Room 的业务错误码转成调用结果。
///
/// 关键区分：传输层失败（controller.Failed()）单独处理为 kUnavailable，
/// 与响应体里的业务错误分开。混在一起会把「房间已结束」误报成「服务不可用」。
RoomCallStatus ToCallStatus(rgbt::room::v1::RoomErrorCode code) {
    switch (code) {
        case rgbt::room::v1::ROOM_ERROR_CODE_UNSPECIFIED:
            return RoomCallStatus::kOk;
        case rgbt::room::v1::ROOM_INVALID_ARGUMENT:
            return RoomCallStatus::kInvalidArgument;
        case rgbt::room::v1::ROOM_NOT_FOUND:
            return RoomCallStatus::kNotFound;
        case rgbt::room::v1::ROOM_ALREADY_FINISHED:
            return RoomCallStatus::kAlreadyFinished;
        case rgbt::room::v1::ROOM_RESULT_PENDING:
            return RoomCallStatus::kResultPending;
        case rgbt::room::v1::ROOM_STORE_UNAVAILABLE:
            return RoomCallStatus::kStoreUnavailable;
        case rgbt::room::v1::ROOM_INTERNAL:
        default:
            return RoomCallStatus::kInternal;
    }
}

}  // namespace

struct BrpcRoomClient::Impl {
    RoomClientOptions options;
    brpc::Channel channel;
    bool healthy = true;

    explicit Impl(RoomClientOptions opts) : options(std::move(opts)) {
        brpc::ChannelOptions channel_options;
        channel_options.timeout_ms = options.timeout_ms;
        channel_options.connect_timeout_ms = options.connect_timeout_ms;
        // 不重试：重试语义由业务层决定，传输层自动重试会让「提交输入」这类
        // 非幂等动作被重复执行。
        channel_options.max_retry = 0;
        const std::string target = options.host + ":" + std::to_string(options.port);
        if (channel.Init(target.c_str(), &channel_options) != 0) {
            // Init 失败只说明目标串不合法，不代表对端不可用。真正的不可用会在
            // 第一次调用时以 controller.Failed() 的形式暴露出来。
            healthy = false;
        }
    }
};

BrpcRoomClient::BrpcRoomClient(RoomClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

BrpcRoomClient::~BrpcRoomClient() = default;

RoomCallStatus BrpcRoomClient::Join(const std::string& room_id, const std::string& player_id,
                                    RoomSnapshot* out_snapshot) {
    rgbt::room::v1::RoomService_Stub stub(&impl_->channel);

    rgbt::room::v1::JoinRoomRequest request;
    request.set_request_id(room_id + ":" + player_id);
    request.set_room_id(room_id);
    request.set_player_id(player_id);
    rgbt::room::v1::JoinRoomResponse response;

    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.JoinRoom(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        return RoomCallStatus::kUnavailable;
    }
    if (out_snapshot != nullptr) {
        *out_snapshot = ToSnapshot(response.room());
    }
    return ToCallStatus(response.error().code());
}

RoomCallStatus BrpcRoomClient::SubmitAttack(const std::string& room_id,
                                            const std::string& player_id,
                                            RoomSnapshot* out_snapshot) {
    rgbt::room::v1::RoomService_Stub stub(&impl_->channel);

    rgbt::room::v1::SubmitInputRequest request;
    // request_id 用「房间:玩家」而不是随机串：本接口不要求幂等键（同一帧的重复
    // 攻击会被服务端合并），用它只是为了在跨服务日志里能直接关联到人和房间。
    request.set_request_id(room_id + ":" + player_id);
    request.set_room_id(room_id);
    request.set_player_id(player_id);
    request.mutable_input()->set_kind(rgbt::room::v1::PlayerInput::KIND_ATTACK);
    rgbt::room::v1::SubmitInputResponse response;

    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.SubmitInput(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        return RoomCallStatus::kUnavailable;
    }
    if (out_snapshot != nullptr) {
        *out_snapshot = ToSnapshot(response.room());
    }
    return ToCallStatus(response.error().code());
}

RoomCallStatus BrpcRoomClient::GetState(const std::string& room_id, RoomSnapshot* out_snapshot) {
    rgbt::room::v1::RoomService_Stub stub(&impl_->channel);

    rgbt::room::v1::GetRoomStateRequest request;
    request.set_request_id(room_id);
    request.set_room_id(room_id);
    rgbt::room::v1::GetRoomStateResponse response;

    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.GetRoomState(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        return RoomCallStatus::kUnavailable;
    }
    if (out_snapshot != nullptr) {
        *out_snapshot = ToSnapshot(response.room());
    }
    return ToCallStatus(response.error().code());
}

RoomCallStatus BrpcRoomClient::GetResult(const std::string& match_id, MatchResultView* out_view) {
    rgbt::room::v1::RoomService_Stub stub(&impl_->channel);

    rgbt::room::v1::GetMatchResultRequest request;
    request.set_request_id(match_id);
    request.set_match_id(match_id);
    rgbt::room::v1::GetMatchResultResponse response;

    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.GetMatchResult(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        return RoomCallStatus::kUnavailable;
    }

    if (out_view != nullptr) {
        *out_view = MatchResultView{};
        // RESULT_PENDING 时 result 为空但 room 有效；成功时 result 有效。
        // 用 error.code 判定，而不是「字段是否为空」——平局的结果本身就是空 winner。
        if (response.has_result()) {
            out_view->has_result = true;
            out_view->result.match_id = response.result().match_id();
            out_view->result.room_id = response.result().room_id();
            out_view->result.winner_id = response.result().winner_id();
            out_view->result.player_count = response.result().player_count();
            out_view->result.started_at_ms = response.result().started_at_ms();
            out_view->result.finished_at_ms = response.result().finished_at_ms();
        }
        if (response.has_room()) {
            out_view->has_room = true;
            out_view->room = ToSnapshot(response.room());
        }
    }

    return ToCallStatus(response.error().code());
}

bool BrpcRoomClient::IsHealthy() {
    // 不做真实往返调用：启动日志里的「可用性」不应该因为一次探测就改变服务的
    // 启动行为。真正的不可用会在第一次请求时以 503 的形式暴露。
    return impl_ != nullptr && impl_->healthy;
}

}  // namespace rgbt::gateway
