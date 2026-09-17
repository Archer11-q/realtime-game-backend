#include "password_hash.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>

namespace rgbt::gateway {
namespace {

/// SHA-256 摘要长度（字节）。
constexpr std::size_t kDigestLength = 32;

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

std::string HashPassword(std::string_view password) {
    std::array<unsigned char, kDigestLength> digest{};
    unsigned int written = 0;

    // 使用 EVP 接口而不是已废弃的 SHA256_* 便捷函数。
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return {};
    }
    std::string result;
    do {
        if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
            break;
        }
        if (EVP_DigestUpdate(context, password.data(), password.size()) != 1) {
            break;
        }
        if (EVP_DigestFinal_ex(context, digest.data(), &written) != 1) {
            break;
        }
        if (written != kDigestLength) {
            break;
        }
        // 转为小写十六进制，长度固定 64，便于比较与日志。
        result.reserve(kDigestLength * 2);
        for (unsigned char byte : digest) {
            result.push_back(kHexDigits[byte >> 4]);
            result.push_back(kHexDigits[byte & 0x0F]);
        }
    } while (false);

    EVP_MD_CTX_free(context);
    return result;
}

bool ConstantTimeEquals(std::string_view lhs, std::string_view rhs) noexcept {
    // 长度不同时直接返回 false：长度本身不是秘密（摘要长度固定）。
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        diff |= static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(rhs[i]);
    }
    return diff == 0;
}

}  // namespace rgbt::gateway
