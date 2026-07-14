#include <array>
#include <string>

#include <gtest/gtest.h>

#include "axklib/utf8.hpp"

TEST(Utf8, AcceptsScalarBoundariesNoncharactersAndEmbeddedNul) {
    for (const auto &value :
         {std::string{"ascii"}, std::string{"\0", 1U},
          std::string{"\xc2\x80", 2U}, std::string{"\xe0\xa0\x80", 3U},
          std::string{"\xef\xb7\x90", 3U}, std::string{"\xf0\x90\x80\x80", 4U},
          std::string{"\xf4\x8f\xbf\xbf", 4U}}) {
        EXPECT_TRUE(axk::text::is_valid_utf8(value));
    }
}

TEST(Utf8, RejectsOverlongTruncatedSurrogateAndOutOfRangeSequences) {
    for (const auto &value :
         {std::string{"\x80", 1U}, std::string{"\xc0\x80", 2U},
          std::string{"\xc2", 1U}, std::string{"\xe0\x80\x80", 3U},
          std::string{"\xed\xa0\x80", 3U}, std::string{"\xf0\x80\x80\x80", 4U},
          std::string{"\xf4\x90\x80\x80", 4U},
          std::string{"\xf5\x80\x80\x80", 4U}}) {
        EXPECT_FALSE(axk::text::is_valid_utf8(value));
    }
}

TEST(Utf8, ConvertsUtf16PairsAndRejectsLoneSurrogates) {
    const auto converted = axk::text::utf16_to_utf8(u"A \u00e4 \U0001f3b5");
    ASSERT_TRUE(converted);
    EXPECT_EQ(*converted, "A \xc3\xa4 \xf0\x9f\x8e\xb5");

    const std::array lone_high{char16_t{0xd800}};
    const std::array lone_low{char16_t{0xdc00}};
    EXPECT_FALSE(
        axk::text::utf16_to_utf8({lone_high.data(), lone_high.size()}));
    EXPECT_FALSE(axk::text::utf16_to_utf8({lone_low.data(), lone_low.size()}));
}

TEST(Utf8, ConvertsPathsExplicitlyAndRejectsEmbeddedNul) {
    constexpr std::string_view utf8_path{"audio/Gr\xc3\xb6\xc3\x9f"
                                         "e.wav"};
    const auto path = axk::text::path_from_utf8(utf8_path);
    ASSERT_TRUE(path);
    EXPECT_EQ(axk::text::path_to_utf8(*path), utf8_path);
    EXPECT_FALSE(axk::text::path_from_utf8(std::string{"a\0b", 3U}));
    EXPECT_FALSE(axk::text::path_from_utf8(std::string{"\xc3\x28", 2U}));
}

TEST(Utf8, BuildsTemporarySiblingWithoutNarrowingTheNativeFilename) {
    constexpr std::string_view target_utf8{"audio/Gr\xc3\xb6\xc3\x9f"
                                           "e \xe9\x9f\xb3.wav"};
    const auto target = axk::text::path_from_utf8(target_utf8);
    ASSERT_TRUE(target);
    const auto temporary =
        axk::text::temporary_sibling(*target, ".alter.1234.tmp");
    ASSERT_TRUE(temporary);
    EXPECT_EQ(axk::text::path_to_utf8(*temporary),
              "audio/.Gr\xc3\xb6\xc3\x9f"
              "e \xe9\x9f\xb3.wav.alter.1234.tmp");
    EXPECT_FALSE(axk::text::temporary_sibling(*target, "../unsafe"));
}
