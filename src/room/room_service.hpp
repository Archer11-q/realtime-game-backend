/// @file room_service.hpp
/// @brief Room/Battle 的 brpc 服务实现。
///
/// 本层只做两件事：**协议转换**和**错误码映射**。房间逻辑全部在 RoomManager 里，
/// 因此服务层几乎没有分支可以出错，也不会把业务规则散落到协议边界上。
/// 这与 src/match/match_service.cpp 的分工一致。

#ifndef RGBT_ROOM_ROOM_SERVICE_HPP
#define RGBT_ROOM_ROOM_SERVICE_HPP

#include <cstdint>
#include <functional>

#include "room.pb.h"
#include "room_manager.hpp"

namespace rgbt::room {

/// 房间服务实现。
class RoomServiceImpl final : public rgbt::room::v1::RoomService {
public:
    /// @param manager 房间管理器，生命周期由调用方保证。
    /// @param clock 当前时间（Unix 毫秒）。默认取系统时钟；注入的目的只是让
    ///        验收与测试能构造确定的时间，生产路径不需要自定义。
    explicit RoomServiceImpl(RoomManager* manager, std::function<std::int64_t()> clock = {});

    void CreateRoom(google::protobuf::RpcController* controller,
                    const rgbt::room::v1::CreateRoomRequest* request,
                    rgbt::room::v1::CreateRoomResponse* response,
                    google::protobuf::Closure* done) override;

    void JoinRoom(google::protobuf::RpcController* controller,
                  const rgbt::room::v1::JoinRoomRequest* request,
                  rgbt::room::v1::JoinRoomResponse* response,
                  google::protobuf::Closure* done) override;

    void SubmitInput(google::protobuf::RpcController* controller,
                     const rgbt::room::v1::SubmitInputRequest* request,
                     rgbt::room::v1::SubmitInputResponse* response,
                     google::protobuf::Closure* done) override;

    void GetRoomState(google::protobuf::RpcController* controller,
                      const rgbt::room::v1::GetRoomStateRequest* request,
                      rgbt::room::v1::GetRoomStateResponse* response,
                      google::protobuf::Closure* done) override;

    void GetMatchResult(google::protobuf::RpcController* controller,
                        const rgbt::room::v1::GetMatchResultRequest* request,
                        rgbt::room::v1::GetMatchResultResponse* response,
                        google::protobuf::Closure* done) override;

    void GetRoomSnapshotsSince(google::protobuf::RpcController* controller,
                               const rgbt::room::v1::GetRoomSnapshotsSinceRequest* request,
                               rgbt::room::v1::GetRoomSnapshotsSinceResponse* response,
                               google::protobuf::Closure* done) override;

    void SetPlayerPresence(google::protobuf::RpcController* controller,
                           const rgbt::room::v1::SetPlayerPresenceRequest* request,
                           rgbt::room::v1::SetPlayerPresenceResponse* response,
                           google::protobuf::Closure* done) override;

private:
    [[nodiscard]] std::int64_t NowMs() const;

    RoomManager* manager_;
    std::function<std::int64_t()> clock_;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_ROOM_SERVICE_HPP
