/// @file room_allocator.hpp
/// @brief 房间分配接口与 Phase 1 的占位实现。
///
/// 为什么需要这个接口（TASK-007 决策 3 选 A）：
///   匹配成功后，同一局的玩家必须拿到**同一个 room_id**，否则客户端无法进入同一
///   房间。但 Room/Battle Service 属 TASK-008，其契约（room.proto）应由拥有房间的
///   那个任务来定型。若在 TASK-007 里提前定义 room.proto，TASK-008 就被迫迁就一个
///   在没有房间语义的情况下拍出来的接口。
///
/// 因此这里只抽象「拿到一个房间号」这一件事：
///   * Phase 1（本文件）：DerivedRoomAllocator，由 match_id 派生 room_id，
///     只保证同一局的双方拿到同一个号，**不产生任何房间状态**。
///   * TASK-008：替换为调用 Room Service 的实现，并在那里引入 room.proto。
///
/// 这个接口刻意保持最小：只有一个方法，且失败用空字符串表达而不是抛异常或返回
/// 复杂错误码——因为 Phase 1 的占位实现不可能失败，多余的错误类型是未经验证的抽象。

#ifndef RGBT_MATCH_ROOM_ALLOCATOR_HPP
#define RGBT_MATCH_ROOM_ALLOCATOR_HPP

#include <string>
#include <string_view>

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
    /// @param match_id 已生成的匹配 ID。占位实现由它派生 room_id；真实实现会把它
    ///        交给 Room Service 作为幂等键，保证重复分配得到同一个房间。
    /// @return 房间号；无法分配时返回空字符串，调用方按失败处理。
    [[nodiscard]] virtual std::string Allocate(std::string_view match_id) = 0;
};

/// @brief Phase 1 占位实现：由 match_id 直接派生 room_id。
///
/// 规则：`room_id = "room-" + match_id`。因为 match_id 一局一个，同一局的双方自然
/// 得到同一个 room_id。**这个号目前不代表任何真实房间**，TASK-008 会替换本实现。
class DerivedRoomAllocator final : public RoomAllocator {
public:
    [[nodiscard]] std::string Allocate(std::string_view match_id) override;
};

}  // namespace rgbt::match

#endif  // RGBT_MATCH_ROOM_ALLOCATOR_HPP
