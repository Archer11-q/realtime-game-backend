#include "common/version.hpp"

#include <string_view>

namespace rgbt::common {

std::string_view VersionString() noexcept {
    return kVersionString;
}

std::int32_t VersionMajor() noexcept {
    return kVersionMajor;
}

std::int32_t VersionMinor() noexcept {
    return kVersionMinor;
}

std::int32_t VersionPatch() noexcept {
    return kVersionPatch;
}

}  // namespace rgbt::common
