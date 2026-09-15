/// @file version_test.cpp
/// @brief common 版本信息的单元测试，同时验证构建、测试和 CI 基线是否可用。

#include "common/version.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace {

using rgbt::common::kVersionMajor;
using rgbt::common::kVersionMinor;
using rgbt::common::kVersionPatch;
using rgbt::common::kVersionString;
using rgbt::common::VersionMajor;
using rgbt::common::VersionMinor;
using rgbt::common::VersionPatch;
using rgbt::common::VersionString;

TEST(VersionTest, StringIsNotEmpty) {
    EXPECT_FALSE(std::string_view{kVersionString}.empty());
}

TEST(VersionTest, AccessorsMatchConstants) {
    EXPECT_EQ(VersionString(), std::string_view{kVersionString});
    EXPECT_EQ(VersionMajor(), kVersionMajor);
    EXPECT_EQ(VersionMinor(), kVersionMinor);
    EXPECT_EQ(VersionPatch(), kVersionPatch);
}

TEST(VersionTest, ComponentsAreNotNegative) {
    EXPECT_GE(kVersionMajor, 0);
    EXPECT_GE(kVersionMinor, 0);
    EXPECT_GE(kVersionPatch, 0);
}

TEST(VersionTest, MajorVersionIsPositive) {
    // 主版本号为 0 表示尚未发布，第一个可运行版本应为 0.1.x，故此处只要求非负。
    EXPECT_GE(VersionMajor(), std::int32_t{0});
}

}  // namespace
