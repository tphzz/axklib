#include <gtest/gtest.h>

#include "http_headers.hpp"

TEST(HttpHeaders, EncodesQuotedAndUnicodeAttachmentNames) {
    EXPECT_EQ(axk::server::attachment_content_disposition("plain.tar"),
              "attachment; filename=\"plain.tar\"; filename*=UTF-8''plain.tar");
    EXPECT_EQ(axk::server::attachment_content_disposition("a\"b\\c.tar"),
              "attachment; filename=\"a\\\"b\\\\c.tar\"; filename*=UTF-8''a%22b%5Cc.tar");
    EXPECT_EQ(axk::server::attachment_content_disposition("klang-\xc3\xa4.wav"),
              "attachment; filename=\"klang-__.wav\"; filename*=UTF-8''klang-%C3%A4.wav");
}

TEST(HttpHeaders, ReplacesControlsInTheAsciiFallback) {
    EXPECT_EQ(axk::server::attachment_content_disposition("bad\r\nname"),
              "attachment; filename=\"bad__name\"; filename*=UTF-8''bad%0D%0Aname");
    EXPECT_EQ(axk::server::attachment_content_disposition({}),
              "attachment; filename=\"download\"; filename*=UTF-8''download");
}
