#include "room_allocator.hpp"

namespace rgbt::match {

std::string DerivedRoomAllocator::Allocate(std::string_view match_id) {
    if (match_id.empty()) {
        return {};
    }
    std::string room_id;
    room_id.reserve(match_id.size() + 5);
    room_id.append("room-");
    room_id.append(match_id);
    return room_id;
}

}  // namespace rgbt::match
