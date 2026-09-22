#include "match_service.hpp"

#include <brpc/closure_guard.h>

#include <chrono>
#include <string>
#include <utility>

namespace rgbt::match {
namespace {

using rgbt::match::v1::MatchErrorCode;
using rgbt::match::v1::MatchState;

/// 队列状态 -> proto 状态。
MatchState ToProtoState(MatchStatusSnapshot::State state) {
    switch (state) {
        case MatchStatusSnapshot::State::kIdle:
            return MatchState::MATCH_STATE_IDLE;
        case MatchStatusSnapshot::State::kQueued:
            return MatchState::MATCH_STATE_QUEUED;
        case MatchStatusSnapshot::State::kMatched:
            return MatchState::MATCH_STATE_MATCHED;
        case MatchStatusSnapshot::State::kTimeout:
            return MatchState::MATCH_STATE_TIMEOUT;
    }
    // proto3 的枚举有 sentinel 值，switch 必须覆盖全部取值，这里作为兜底。
    return MatchState::MATCH_STATE_UNSPECIFIED;
}

}  // namespace

std::int64_t NowUnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

MatchServiceImpl::MatchServiceImpl(MatchQueue* queue, std::function<std::int64_t()> clock)
    : queue_(queue),
      clock_(clock ? std::move(clock) : std::function<std::int64_t()>(NowUnixMillis)) {}

bool MatchServiceImpl::FillError(rgbt::match::v1::MatchError* error,
                                 rgbt::match::v1::MatchErrorCode code, const std::string& reason,
                                 const std::string& message, const std::string& request_id) {
    if (error != nullptr) {
        error->set_code(code);
        error->set_reason(reason);
        error->set_message(message);
        error->set_request_id(request_id);
    }
    return false;
}

void MatchServiceImpl::FillStatus(const MatchStatusSnapshot& snapshot,
                                  rgbt::match::v1::MatchStatus* out) {
    if (out == nullptr) {
        return;
    }
    out->set_state(ToProtoState(snapshot.state));
    out->set_request_id(snapshot.request_id);
    out->set_match_id(snapshot.match_id);
    out->set_room_id(snapshot.room_id);
    for (const std::string& player_id : snapshot.player_ids) {
        out->add_player_ids(player_id);
    }
    out->set_queued_at_ms(snapshot.queued_at_ms);
    out->set_queue_size(static_cast<std::int32_t>(snapshot.queue_size));
}

void MatchServiceImpl::EnqueueMatch(::google::protobuf::RpcController* /*controller*/,
                                    const rgbt::match::v1::EnqueueMatchRequest* request,
                                    rgbt::match::v1::EnqueueMatchResponse* response,
                                    ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string request_id = request->request_id();
    const std::string player_id = request->player_id();
    const std::int64_t now_ms = clock_();

    const EnqueueOutcome outcome = queue_->Enqueue(player_id, request_id, now_ms);
    switch (outcome) {
        case EnqueueOutcome::kQueued:
            // 入队成功。注意状态可能是 QUEUED，也可能在同一次调用内就变成 MATCHED
            // （第二个入队的人会立刻配成局），因此这里必须回读真实状态而不是写死。
            FillStatus(queue_->GetStatus(player_id, now_ms), response->mutable_status());
            return;
        case EnqueueOutcome::kAlreadyQueued:
            // 幂等：把当前状态返回给调用方，让它自己判断是等待还是领取结果。
            FillError(response->mutable_error(), MatchErrorCode::MATCH_ALREADY_QUEUED,
                      "already_queued", "玩家已在匹配队列或已有匹配结果", request_id);
            FillStatus(queue_->GetStatus(player_id, now_ms), response->mutable_status());
            return;
        case EnqueueOutcome::kQueueFull:
            FillError(response->mutable_error(), MatchErrorCode::MATCH_QUEUE_FULL, "queue_full",
                      "匹配队列已满，请稍后重试", request_id);
            return;
        case EnqueueOutcome::kInvalidArgument:
            FillError(response->mutable_error(), MatchErrorCode::MATCH_INVALID_ARGUMENT,
                      "invalid_argument", "玩家标识或幂等键不合法", request_id);
            return;
    }
}

void MatchServiceImpl::GetMatchStatus(::google::protobuf::RpcController* /*controller*/,
                                      const rgbt::match::v1::GetMatchStatusRequest* request,
                                      rgbt::match::v1::GetMatchStatusResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // 查询不产生副作用，空 player_id 返回全空的 IDLE 状态即可，不作为错误：
    // 这样调用方不必为「刚登录还没匹配过」这一个正常情形写错误分支。
    FillStatus(queue_->GetStatus(request->player_id(), clock_()), response->mutable_status());
}

void MatchServiceImpl::CancelMatch(::google::protobuf::RpcController* /*controller*/,
                                   const rgbt::match::v1::CancelMatchRequest* request,
                                   rgbt::match::v1::CancelMatchResponse* response,
                                   ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string request_id = request->request_id();
    const std::string player_id = request->player_id();
    const std::int64_t now_ms = clock_();

    // 取消是幂等操作：不在队列中（返回 false）也视为成功，与登出保持一致。
    // 因此这里不使用返回值判断成败，只回读最终状态。
    queue_->Cancel(player_id, now_ms);
    FillStatus(queue_->GetStatus(player_id, now_ms), response->mutable_status());
}

}  // namespace rgbt::match
