#include <algorithm>
#include <cctype>

#include <gtest/gtest.h>

#include "axklib/application/secure_random.hpp"

TEST(SecureRandom, ProducesDistinctLowercaseHexIdentifiersFromTheOperatingSystem) {
    const auto first = axk::app::secure_random_hex(32U);
    const auto second = axk::app::secure_random_hex(32U);

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(first->size(), 64U);
    EXPECT_EQ(second->size(), 64U);
    EXPECT_NE(*first, *second);
    EXPECT_TRUE(std::ranges::all_of(*first, [](unsigned char character) {
        return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
    }));
}
