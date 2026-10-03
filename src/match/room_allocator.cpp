#include "room_allocator.hpp"

#include <string>
#include <vector>

namespace rgbt::match {

std::string DerivedRoomAllocator::Allocate(std::string_view match_id,
                                           const std::vector<std::string>& /*player_ids*/,
                                           std::string_view /*request_id*/) {
    if (match_id.empty()) {
        return {};
    }
    // 占位实现不使用玩家列表，也不使用 request_id：它只保证同一局的双方拿到同一个号，
    // 而这个性质由「match_id 一局一个」即可保证。这里保持不依赖 trace 是有意的——
    // 它本来就不产生任何房间状态，也就没有任何跨服务链路需要串起来。
    std::string room_id;
    room_id.reserve(match_id.size() + 5);
    room_id.append("room-");
    room_id.append(match_id);
    return room_id;
}

}  // namespace rgbt::match
