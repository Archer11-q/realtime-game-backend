#include "brpc_match_client.hpp"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/metrics.hpp"
#include "match.pb.h"

namespace rgbt::gateway {
namespace {

/// TASK-019：把一次服务间调用的结果记到指标上。
///
/// 为什么包装 `return XxxCallStatus::...` 而不是改每个调用点：这是真正的 RPC 层，
/// 每个方法都以一个 CallStatus 收敛。包装 return 保证**零遗漏**——漏一个调用点
/// 不会有任何报错，只会让"错误率"这个指标悄悄偏低，而那正是排障时最依赖的数字。
///
/// `ok` 只认 `kOk`：其余取值（含 `kResultPending`、`kNotAMember` 这类业务性拒绝）
/// 对"服务间调用是否成功"这个问题的答案都是"没成功"。
void RecordRpcCall(bool ok) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, rgbt::common::CounterHandle> cache;
    const std::string key = ok ? "ok" : "failed";

    rgbt::common::CounterHandle handle;
    {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cache.find(key);
        if (it == cache.end()) {
            handle = rgbt::common::Metrics().Counter(rgbt::common::kMetricRpcCallsTotal,
                                                     "Gateway 发起的服务间 RPC 调用数",
                                                     {{"target", "match"}, {"outcome", key}});
            cache.emplace(key, handle);
        } else {
            handle = it->second;
        }
    }
    handle.Add();
}

/// 把 Match 的快照转成 Gateway 自己的结构。
MatchSnapshot ToSnapshot(const rgbt::match::v1::MatchStatus& status) {
    MatchSnapshot snapshot;
    switch (status.state()) {
        case rgbt::match::v1::MATCH_STATE_QUEUED:
            snapshot.state = MatchState::kQueued;
            break;
        case rgbt::match::v1::MATCH_STATE_MATCHED:
            snapshot.state = MatchState::kMatched;
            break;
        case rgbt::match::v1::MATCH_STATE_TIMEOUT:
            snapshot.state = MatchState::kTimeout;
            break;
        case rgbt::match::v1::MATCH_STATE_IDLE:
        case rgbt::match::v1::MATCH_STATE_UNSPECIFIED:
        default:
            snapshot.state = MatchState::kIdle;
            break;
    }
    snapshot.match_id = status.match_id();
    snapshot.room_id = status.room_id();
    snapshot.player_ids.assign(status.player_ids().begin(), status.player_ids().end());
    snapshot.queued_at_ms = status.queued_at_ms();
    snapshot.queue_size = status.queue_size();
    return snapshot;
}

/// 把 Match 的业务错误码转成调用结果。
///
/// MATCH_ALREADY_QUEUED 映射为 kOk：对 Gateway 而言「玩家已在队列」不是错误，
/// 后续会用 Match 返回的当前状态正常回 200，这是幂等语义的一部分。
MatchCallStatus ToCallStatus(const rgbt::match::v1::MatchError& error) {
    switch (error.code()) {
        case rgbt::match::v1::MATCH_ERROR_CODE_UNSPECIFIED:
        case rgbt::match::v1::MATCH_ALREADY_QUEUED:
            RecordRpcCall(true);
            return MatchCallStatus::kOk;
        case rgbt::match::v1::MATCH_INVALID_ARGUMENT:
            RecordRpcCall(false);
            return MatchCallStatus::kInvalidArgument;
        case rgbt::match::v1::MATCH_QUEUE_FULL:
            RecordRpcCall(false);
            return MatchCallStatus::kQueueFull;
        case rgbt::match::v1::MATCH_INTERNAL:
        default:
            RecordRpcCall(false);
            return MatchCallStatus::kInternal;
    }
}

}  // namespace

struct BrpcMatchClient::Impl {
    MatchClientOptions options;
    brpc::Channel channel;

    explicit Impl(MatchClientOptions opts) : options(std::move(opts)) {
        brpc::ChannelOptions channel_options;
        channel_options.timeout_ms = options.timeout_ms;
        channel_options.connect_timeout_ms = options.connect_timeout_ms;
        // 单连接 + 短超时：Phase 1 只有 Gateway 一个调用方，不需要连接池。
        channel_options.max_retry = 0;
        const std::string target = options.host + ":" + std::to_string(options.port);
        if (channel.Init(target.c_str(), &channel_options) != 0) {
            // Init 失败只说明目标串不合法，不代表对端不可用。真正的不可用会在
            // 第一次调用时以 controller.Failed() 的形式暴露出来。
            healthy_ = false;
        }
    }

    bool healthy_ = true;
};

BrpcMatchClient::BrpcMatchClient(MatchClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

BrpcMatchClient::~BrpcMatchClient() = default;

MatchCallStatus BrpcMatchClient::Enqueue(const std::string& player_id,
                                         const std::string& request_id,
                                         MatchSnapshot* out_snapshot) {
    rgbt::match::v1::MatchService_Stub stub(&impl_->channel);

    rgbt::match::v1::EnqueueMatchRequest request;
    request.set_player_id(player_id);
    request.set_request_id(request_id);
    rgbt::match::v1::EnqueueMatchResponse response;

    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.EnqueueMatch(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        RecordRpcCall(false);
        return MatchCallStatus::kUnavailable;
    }
    if (out_snapshot != nullptr) {
        *out_snapshot = ToSnapshot(response.status());
    }
    return ToCallStatus(response.error());
}

MatchCallStatus BrpcMatchClient::GetStatus(const std::string& player_id,
                                           MatchSnapshot* out_snapshot) {
    rgbt::match::v1::MatchService_Stub stub(&impl_->channel);

    rgbt::match::v1::GetMatchStatusRequest request;
    request.set_player_id(player_id);
    rgbt::match::v1::GetMatchStatusResponse response;

    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.GetMatchStatus(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        RecordRpcCall(false);
        return MatchCallStatus::kUnavailable;
    }
    if (out_snapshot != nullptr) {
        *out_snapshot = ToSnapshot(response.status());
    }
    return ToCallStatus(response.error());
}

MatchCallStatus BrpcMatchClient::Cancel(const std::string& player_id, const std::string& request_id,
                                        MatchSnapshot* out_snapshot) {
    rgbt::match::v1::MatchService_Stub stub(&impl_->channel);

    rgbt::match::v1::CancelMatchRequest request;
    request.set_player_id(player_id);
    request.set_request_id(request_id);
    rgbt::match::v1::CancelMatchResponse response;

    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.CancelMatch(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        RecordRpcCall(false);
        return MatchCallStatus::kUnavailable;
    }
    if (out_snapshot != nullptr) {
        *out_snapshot = ToSnapshot(response.status());
    }
    return ToCallStatus(response.error());
}

bool BrpcMatchClient::IsHealthy() {
    // 不做真实往返调用：启动日志里的「可用性」不应该因为一次探测就改变服务的
    // 启动行为。真正的不可用会在第一次请求时以 503 的形式暴露。
    return impl_ != nullptr && impl_->healthy_;
}

}  // namespace rgbt::gateway
