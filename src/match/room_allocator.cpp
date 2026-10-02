#include "room_allocator.hpp"

#include <string>
#include <vector>

namespace rgbt::match {

std::string DerivedRoomAllocator::Allocate(std::string_view match_id,
                                           const std::vector<std::string>& /*player_ids*/) {
    if (match_id.empty()) {
        return {};
    }
    // 占位实现不使用玩家列表：它只保证同一局的双方拿到同一个号，
    // 而这个性质由「match_id 一局一个」即可保证。
    std::string room_id;
    room_id.reserve(match_id.size() + 5);
    room_id.append("room-");
    room_id.append(match_id);
    return room_id;
}

}  // namespace rgbt::match
