#include <string_view>

#include <gtest/gtest.h>

#include "axklib/version.hpp"

TEST(Version, IsStableAndNonEmpty) { EXPECT_EQ(axk::version(), std::string_view{"0.1.0"}); }
