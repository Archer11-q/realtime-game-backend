/// @file room_allocator.hpp
/// @brief 房间分配接口。
///
/// 为什么需要这个接口（TASK-007 决策 3 选 A）：
///   匹配成功后，同一局的玩家必须拿到**同一个 room_id**，否则客户端无法进入同一
///   房间。TASK-007 期间 Room/Battle Service 尚不存在，因此只抽象「拿到一个房间号」
///   这一件事，用一个不产生任何房间状态的占位实现。
///
/// TASK-008 起：真实实现为 BrpcRoomAllocator（见 brpc_room_allocator.hpp），
/// 它调用 RoomService.CreateRoom。**接口因此增加了 player_ids 参数**——房间需要
/// 知道本局都有谁，才能校验加入者的身份；只传 match_id 的话，Room 无法判断
/// 谁是这一局的人。
///
/// 失败用空字符串表达而不是抛异常或返回复杂错误码：调用方的处理方式只有一种
/// ——「这次配对不成立，把玩家放回队列」。多余的错误类型是未经验证的抽象。

#ifndef RGBT_MATCH_ROOM_ALLOCATOR_HPP
#define RGBT_MATCH_ROOM_ALLOCATOR_HPP

#include <string>
#include <string_view>
#include <vector>

namespace rgbt::match {

/// 房间分配接口。
class RoomAllocator {
public:
    RoomAllocator() = default;
    RoomAllocator(const RoomAllocator&) = delete;
    RoomAllocator& operator=(const RoomAllocator&) = delete;
    RoomAllocator(RoomAllocator&&) = delete;
    RoomAllocator& operator=(RoomAllocator&&) = delete;
    virtual ~RoomAllocator() = default;

    /// @brief 为一次匹配结果分配房间号。
    /// @param match_id 已生成的匹配 ID。实现应把它交给 Room Service 作为**幂等键**，
    ///        保证重复分配得到同一个房间。
    /// @param player_ids 本局玩家。Room 据此建立成员名单。
    /// @param request_id 这次配对所属请求的关联 id（TASK-021）。实现应把它**透传**
    ///        给 Room，使 Room 的 `room_created` 与 Gateway 的 `request_done`、
    ///        Match 的 `match_enqueued` 能用同一个 trace 串起来。
    ///
    ///        **为什么必须单独一个参数、而不是复用 match_id**：TASK-008 时期这里写的是
    ///        `request_id = match_id`——一个由服务端生成的伪 id。于是 Room 侧记下的
    ///        trace 在 Gateway 与 Match 的日志里**根本不存在**，"匹配 → 建房间"这条
    ///        跨服务链路实际上是断的（TASK-021 用实测数据确认）。
    ///
    ///        **配对是异步的**，因此这里的取值不是"发起配对的客户端"，而是
    ///        "该组队首玩家的 request_id"——由 `MatchQueue` 决定
    ///        （见其 `TakePairGroupsLocked`）。空字符串表示没有可用的关联 id，
    ///        此时实现**不得编造一个**（宁缺勿假，与 logging.hpp 的口径一致）。
    /// @return 房间号；无法分配时返回空字符串，调用方按失败处理。
    ///
    /// **调用约定**：本方法会发起网络调用，调用方**不得在持有自己的锁时调用它**，
    /// 否则一次对端超时会把整个匹配队列卡住。
    [[nodiscard]] virtual std::string Allocate(std::string_view match_id,
                                               const std::vector<std::string>& player_ids,
                                               std::string_view request_id) = 0;
};

/// @brief 占位实现：由 match_id 直接派生 room_id。
///
/// 规则：`room_id = "room-" + match_id`。因为 match_id 一局一个，同一局的双方自然
/// 得到同一个 room_id。**这个号不代表任何真实房间**，只用于不关心房间的测试。
/// 生产路径使用 BrpcRoomAllocator。
class DerivedRoomAllocator final : public RoomAllocator {
public:
    [[nodiscard]] std::string Allocate(std::string_view match_id,
                                       const std::vector<std::string>& player_ids,
                                       std::string_view request_id) override;
};

}  // namespace rgbt::match

#endif  // RGBT_MATCH_ROOM_ALLOCATOR_HPP
