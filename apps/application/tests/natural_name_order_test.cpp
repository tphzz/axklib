#include "axklib/application/natural_name_order.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

TEST(NaturalNameOrder, UsesUnicodeNumericOrderingAndExactTieBreaks) {
    auto order = axk::app::NaturalNameOrder::create();
    ASSERT_TRUE(order) << order.error().message;
    const std::vector<std::string> expected{"ACE",
                                            "Disk02",
                                            "disk2",
                                            "Disk10",
                                            "Eclair",
                                            "eclair",
                                            "e\xcc\x81"
                                            "clair",
                                            "\xc3\xa9"
                                            "clair",
                                            "industrialkit",
                                            "norddrms",
                                            "ROKTON",
                                            "Virus",
                                            "\xce\x91",
                                            "\xce\xb1",
                                            "\xce\xb2"};
    std::vector<axk::app::NaturalNameKey> keys;
    for (auto it = expected.rbegin(); it != expected.rend(); ++it) {
        auto key = order->key(*it);
        ASSERT_TRUE(key) << key.error().message;
        keys.push_back(std::move(*key));
    }
    std::ranges::sort(keys);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        EXPECT_EQ(keys[index].original, expected[index]);
        EXPECT_EQ(keys[index], keys[index]);
        for (std::size_t next = index + 1U; next < keys.size(); ++next) {
            EXPECT_LT(keys[index], keys[next]);
            EXPECT_GT(keys[next], keys[index]);
        }
    }
}

TEST(NaturalNameOrder, RetainsPunctuationAndCanonicalSpellings) {
    auto order = axk::app::NaturalNameOrder::create();
    ASSERT_TRUE(order);
    const auto plain = order->key("ab");
    const auto punctuation = order->key("a-b");
    const auto composed = order->key("\xc3\xa9");
    const auto decomposed = order->key("e\xcc\x81");
    ASSERT_TRUE(plain);
    ASSERT_TRUE(punctuation);
    ASSERT_TRUE(composed);
    ASSERT_TRUE(decomposed);
    EXPECT_NE(plain->collation, punctuation->collation);
    EXPECT_EQ(composed->collation, decomposed->collation);
    EXPECT_NE(*composed, *decomposed);
    EXPECT_EQ(composed->original, "\xc3\xa9");
}

TEST(NaturalNameOrder, RejectsInvalidUtf8WithoutReplacement) {
    auto order = axk::app::NaturalNameOrder::create();
    ASSERT_TRUE(order);
    EXPECT_FALSE(order->key("\xff"));
    EXPECT_FALSE(order->key("\xc3"));
}

TEST(NaturalNameOrder, ComparesLongNumbersWithoutIntegerConversion) {
    auto order = axk::app::NaturalNameOrder::create();
    ASSERT_TRUE(order);
    const auto smaller = order->key("Disk" + std::string(70U, '9'));
    const auto larger = order->key("disk1" + std::string(70U, '0'));
    ASSERT_TRUE(smaller);
    ASSERT_TRUE(larger);
    EXPECT_LT(*smaller, *larger);
}
