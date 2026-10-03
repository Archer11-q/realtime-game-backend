/// @file brpc_room_client.hpp
/// @brief RoomClient 的 brpc 实现。
///
/// 与 BrpcMatchClient 的结构保持一致：pimpl 隐藏 brpc 的 channel 与生成代码，
/// 头文件不泄漏 room.pb.h。

#ifndef RGBT_GATEWAY_BRPC_ROOM_CLIENT_HPP
#define RGBT_GATEWAY_BRPC_ROOM_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "room_client.hpp"

namespace rgbt::gateway {

/// 连接参数。
struct RoomClientOptions {
    std::string host = "127.0.0.1";
    std::int32_t port = 8083;

    /// 单次 RPC 的超时。设得比客户端超时短，保证依赖故障表现为快速失败的 503，
    /// 而不是让浏览器请求一直挂着。
    std::int32_t timeout_ms = 500;

    /// 建立连接的超时。
    std::int32_t connect_timeout_ms = 200;
};

class BrpcRoomClient final : public RoomClient {
public:
    explicit BrpcRoomClient(RoomClientOptions options);
    ~BrpcRoomClient() override;

    BrpcRoomClient(const BrpcRoomClient&) = delete;
    BrpcRoomClient& operator=(const BrpcRoomClient&) = delete;

    RoomCallStatus Join(const std::string& room_id, const std::string& player_id,
                        const std::string& request_id, RoomSnapshot* out_snapshot) override;

    RoomCallStatus SubmitAttack(const std::string& room_id, const std::string& player_id,
                                const std::string& request_id, RoomSnapshot* out_snapshot) override;

    RoomCallStatus GetState(const std::string& room_id, const std::string& request_id,
                            RoomSnapshot* out_snapshot) override;

    RoomCallStatus GetResult(const std::string& match_id, const std::string& request_id,
                             MatchResultView* out_view) override;

    RoomCallStatus GetSnapshotsSince(const std::string& room_id, std::int64_t since_frame,
                                     const std::string& request_id,
                                     SnapshotRange* out_range) override;

    RoomCallStatus SetPresence(const std::string& room_id, const std::string& player_id,
                               bool online, const std::string& request_id,
                               RoomSnapshot* out_snapshot) override;

    [[nodiscard]] bool IsHealthy() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_BRPC_ROOM_CLIENT_HPP
