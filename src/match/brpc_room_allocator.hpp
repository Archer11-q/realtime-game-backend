/// @file brpc_room_allocator.hpp
/// @brief RoomAllocator 的 brpc 实现：调用 RoomService.CreateRoom。
///
/// 与 src/gateway/brpc_match_client.hpp 的结构一致：pimpl 隐藏 channel 与生成代码，
/// 头文件不泄漏 room.pb.h。

#ifndef RGBT_MATCH_BRPC_ROOM_ALLOCATOR_HPP
#define RGBT_MATCH_BRPC_ROOM_ALLOCATOR_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "room_allocator.hpp"

namespace rgbt::match {

/// Room Service 连接参数。
struct RoomAllocatorOptions {
    std::string host = "127.0.0.1";
    std::int32_t port = 8083;

    /// 单次 RPC 的超时。这个值直接决定了「配对到出结果」的最坏延迟，
    /// 因此刻意设得比 Gateway 的客户端超时短。
    std::int32_t timeout_ms = 500;

    std::int32_t connect_timeout_ms = 200;
};

class BrpcRoomAllocator final : public RoomAllocator {
public:
    explicit BrpcRoomAllocator(RoomAllocatorOptions options);
    ~BrpcRoomAllocator() override;

    BrpcRoomAllocator(const BrpcRoomAllocator&) = delete;
    BrpcRoomAllocator& operator=(const BrpcRoomAllocator&) = delete;

    /// @return 成功时返回 room_id；Room 不可用或拒绝时返回空字符串。
    ///
    /// **幂等**：request 里带上 match_id，Room 侧以它为幂等键。因此本方法即使因为
    /// 超时被重试，也不会为同一局造出第二个房间。
    ///
    /// `request_id` 只用于**可观测性**（TASK-021），与幂等键是两件事：幂等靠
    /// match_id，trace 靠上游传来的 request_id。把两者混用会让日志里出现一个
    /// 在其它服务中根本查不到的 id（TASK-008 起的实际缺陷）。
    [[nodiscard]] std::string Allocate(std::string_view match_id,
                                       const std::vector<std::string>& player_ids,
                                       std::string_view request_id) override;

    /// @brief Room 当前是否可用。用于启动日志，不做真实往返调用。
    [[nodiscard]] bool IsHealthy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rgbt::match

#endif  // RGBT_MATCH_BRPC_ROOM_ALLOCATOR_HPP
