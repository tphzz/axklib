#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "axklib/version.hpp"

TEST(Version, KeepsSemanticVersionSeparateFromSourceIdentity) {
    EXPECT_EQ(axk::version(), std::string_view{"0.1.0"});
    const auto build = axk::current_build_info();
    ASSERT_NE(build.source_identity, nullptr);
    ASSERT_NE(build.package_basename, nullptr);
    ASSERT_NE(build.git_tag, nullptr);
    ASSERT_NE(build.git_branch, nullptr);
    ASSERT_NE(build.git_sha_short, nullptr);
    const std::string_view source_identity{build.source_identity};
    const std::string package_basename{build.package_basename};
    EXPECT_FALSE(source_identity.empty());
    EXPECT_EQ(package_basename, "axklib-" + std::string{source_identity});
    EXPECT_FALSE(std::string_view{build.git_sha_short}.empty());
    EXPECT_EQ(source_identity.ends_with("-mod"), build.is_dirty);
    EXPECT_FALSE(std::string_view{build.is_tagged_release ? build.git_tag : build.git_branch}.empty());
    EXPECT_EQ(std::string_view{build.git_tag}.empty(), !build.is_tagged_release);
}
