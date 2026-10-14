#include <string>

#include <gtest/gtest.h>

#include "path_parameters.hpp"

TEST(PathParameters, DecodesOpaqueIdsExactlyOnceWithoutFormEncoding) {
    std::string id{"tx16w%3Afile%2Fname%2Bspace+%25%252F%20%C3%A4"};
    ASSERT_TRUE(axk::server::detail::decode_path_parameter(id));
    EXPECT_EQ(id, "tx16w:file/name+space+%%2F \xc3\xa4");
}

TEST(PathParameters, RejectsMalformedEncodingControlsAndInvalidUtf8) {
    for (const auto *input : {"%", "%0", "%gg", "%00", "%1f", "%7F", "%c0%af", "%ff", "%e2%82"}) {
        std::string value{input};
        EXPECT_FALSE(axk::server::detail::decode_path_parameter(value)) << input;
        EXPECT_EQ(value, input);
    }
}
