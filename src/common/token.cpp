#include "common/token.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string_view>

namespace rgbt::common {
namespace {

/// 每个随机数贡献的字符数。取 6 位（64 种取值）正好覆盖 64 字符的字母表。
constexpr std::size_t kCharsPerDraw = 6;
constexpr std::uint64_t kDrawMask = (1ULL << kCharsPerDraw) - 1;

constexpr std::size_t kMinLength = 1;
constexpr std::size_t kMaxLength = 256;

}  // namespace

std::string GenerateToken(std::size_t length) {
    const std::size_t target = std::clamp(length, kMinLength, kMaxLength);
    const std::string_view alphabet{kTokenAlphabet};

    // random_device 用于播种，保证不同进程间不产生相同序列。
    std::random_device seed_source;
    std::mt19937_64 engine(seed_source());

    std::string token;
    token.reserve(target);
    while (token.size() < target) {
        const std::uint64_t draw = engine();
        // 每次取 6 位，逐字符消费，避免浪费随机数。
        for (std::size_t offset = 0; offset < 64 && token.size() < target;
             offset += kCharsPerDraw) {
            const auto index = static_cast<std::size_t>((draw >> offset) & kDrawMask);
            token.push_back(alphabet[index]);
        }
    }
    return token;
}

bool IsValidTokenFormat(const std::string& token) noexcept {
    if (token.size() < kMinLength || token.size() > kMaxLength) {
        return false;
    }
    const std::string_view alphabet{kTokenAlphabet};
    return std::all_of(token.begin(), token.end(),
                       [alphabet](char c) { return alphabet.find(c) != std::string_view::npos; });
}

}  // namespace rgbt::common
