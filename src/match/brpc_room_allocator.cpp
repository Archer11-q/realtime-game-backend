#include "brpc_room_allocator.hpp"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include <cstdio>
#include <string>
#include <utility>

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
                                        const std::vector<std::string>& player_ids) {
    if (match_id.empty() || player_ids.empty()) {
        return {};
    }

    rgbt::room::v1::RoomService_Stub stub(&impl_->channel);

    rgbt::room::v1::CreateRoomRequest request;
    request.set_request_id(std::string(match_id));
    request.set_match_id(std::string(match_id));
    for (const std::string& player_id : player_ids) {
        request.add_player_ids(player_id);
    }

    rgbt::room::v1::CreateRoomResponse response;
    brpc::Controller controller;
    controller.set_timeout_ms(impl_->options.timeout_ms);
    stub.CreateRoom(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
        // 传输失败或对端未启动。返回空串让调用方把玩家放回队列，
        // 而不是在这里重试——重试会延长持锁之外的处理时间，也会掩盖故障。
        std::fprintf(stderr, "[match] 调用 Room 创建房间失败：match_id=%s err=%s\n",
                     request.match_id().c_str(), controller.ErrorText().c_str());
        return {};
    }
    if (response.error().code() != rgbt::room::v1::ROOM_ERROR_CODE_UNSPECIFIED) {
        std::fprintf(stderr, "[match] Room 拒绝创建房间：match_id=%s reason=%s\n",
                     request.match_id().c_str(), response.error().reason().c_str());
        return {};
    }
    return response.room_id();
}

bool BrpcRoomAllocator::IsHealthy() const {
    return impl_ != nullptr && impl_->healthy;
}

}  // namespace rgbt::match
