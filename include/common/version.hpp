/// @file version.hpp
/// @brief 项目版本信息。
///
/// 版本号的数值来自构建系统注入的编译期宏，保证代码中的版本与 CMake 的
/// project(VERSION ...) 只有一处来源。
///
/// 注意：不要在本文件使用 `@VAR@` 形式的模板占位符。clang-format 会把
/// `@VAR@` 识别为运算符并在内部插入空格，导致模板替换失效。数值一律通过
/// `RGBT_VERSION_*` 宏传入。

#ifndef RGBT_COMMON_VERSION_HPP
#define RGBT_COMMON_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace rgbt::common {

/// 项目版本的字符串形式，例如 "0.1.0"。
inline constexpr const char* kVersionString = RGBT_VERSION_STRING;

/// 主版本号，可在编译期用于接口兼容性判断。
inline constexpr std::int32_t kVersionMajor = (RGBT_VERSION_MAJOR);

/// 次版本号。
inline constexpr std::int32_t kVersionMinor = (RGBT_VERSION_MINOR);

/// 修订号。
inline constexpr std::int32_t kVersionPatch = (RGBT_VERSION_PATCH);

/// @brief 返回项目版本字符串。
[[nodiscard]] std::string_view VersionString() noexcept;

/// @brief 返回主版本号。
[[nodiscard]] std::int32_t VersionMajor() noexcept;

/// @brief 返回次版本号。
[[nodiscard]] std::int32_t VersionMinor() noexcept;

/// @brief 返回修订号。
[[nodiscard]] std::int32_t VersionPatch() noexcept;

}  // namespace rgbt::common

#endif  // RGBT_COMMON_VERSION_HPP
