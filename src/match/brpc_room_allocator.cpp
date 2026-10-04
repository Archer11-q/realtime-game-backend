#include "brpc_room_allocator.hpp"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include <cstdio>
#include <string>
#include <utility>

#include "common/logging.hpp"
#include "room.pb.h"

namespace rgbt::match {

struct BrpcRoomAllocator::Impl {
    RoomAllocatorOptions options;
    brpc::Channel channel;
    bool healthy = true;

    explicit Impl(RoomAllocatorOptions opts) : options(std::move(opts)) {
        brpc::ChannelOptions channel_options;
        channel_options.timeout_ms = options.timeout_ms;
        channel_options.connect_timeout_ms = options.connect_timeout_ms;
        // max_retry = 0：CreateRoom 本身是幂等的（以 match_id 为键），
        // 但重试策略应由业务层决定，这样「失败后把玩家放回队列」的路径才是可测的。
        channel_options.max_retry = 0;
        const std::string target = options.host + ":" + std::to_string(options.port);
        if (channel.Init(target.c_str(), &channel_options) != 0) {
            healthy = false;
        }
    }
};

BrpcRoomAllocator::BrpcRoomAllocator(RoomAllocatorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

BrpcRoomAllocator::~BrpcRoomAllocator() = default;

std::string BrpcRoomAllocator::Allocate(std::string_view match_id,
                                        const std::vector<std::string>& player_ids,
                                        std::string_view request_id) {
    if (match_id.empty() || player_ids.empty()) {
        return {};
    }

    rgbt::room::v1::RoomService_Stub stub(&impl_->channel);

    rgbt::room::v1::CreateRoomRequest request;
    // TASK-021：`request_id` 是**上游传来的关联 id**（该组队首玩家那次入队请求的 id），
    // 与幂等键 `match_id` 是两件事。
    //
    // 修复前这里写的是 `set_request_id(std::string(match_id))`：Room 的 `room_created`
    // 因此记录了一个在 Gateway 与 Match 日志里都不存在的 id，"匹配 → 建房间"这条
    // 跨服务链路实际上是断的。**幂等键仍然是 match_id**，所以换掉它不影响任何
    // 幂等行为——只是日志里终于能串起来了（这就是 TASK-021 要解决的事）。
    request.set_request_id(std::string(request_id));
    request.set_match_id(std::string(match_id));
    for (const std::string& player_id : player_ids) {
        request.add_player_ids(player_id);
    }

    rgbt::room::v1::CreateRoomResponse response;
    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.CreateRoom(&controller, &request, &response, nullptr);

    // 失败日志沿用同一个 trace（而不是 match_id）：此时 Room 侧可能什么都没写，
    // "Match 知道这次调用失败了"与"Room 那边有没有记录"要能在同一条链上对上。
    // 另外把 match_id 作为独立字段保留——它的作用是定位是哪一局，
    // 在队列里 match_id 与 trace 不再混为一谈。
    const std::string trace(request_id);
    if (controller.Failed()) {
        // 传输失败或对端未启动。返回空串让调用方把玩家放回队列，
        // 而不是在这里重试——重试会延长持锁之外的处理时间，也会掩盖故障。
        rgbt::common::LogError("create_room_call_failed", trace,
                               {{"match", std::string(match_id)}, {"err", controller.ErrorText()}});
        return {};
    }
    if (response.error().code() != rgbt::room::v1::ROOM_ERROR_CODE_UNSPECIFIED) {
        rgbt::common::LogError(
            "create_room_rejected", trace,
            {{"match", std::string(match_id)}, {"reason", response.error().reason()}});
        return {};
    }
    return response.room_id();
}

bool BrpcRoomAllocator::IsHealthy() const {
    return impl_ != nullptr && impl_->healthy;
}

}  // namespace rgbt::match
