#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"

TEST(CheckedArithmetic, AddsAndAlignsWithoutOverflow) {
  EXPECT_EQ(axk::checked_add(10, 20), 30U);
  EXPECT_EQ(axk::align_up(16, 8), 16U);
  EXPECT_EQ(axk::align_up(17, 8), 24U);
  EXPECT_EQ(axk::checked_subtract(20, 10), 10U);
  EXPECT_EQ(axk::checked_multiply(20, 10), 200U);

  const auto overflow = axk::checked_add(std::numeric_limits<std::uint64_t>::max(), 1);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().code, axk::ErrorCode::integer_overflow);
  EXPECT_FALSE(axk::checked_subtract(0, 1));
  EXPECT_FALSE(axk::checked_multiply(std::numeric_limits<std::uint64_t>::max(), 2));

  const auto invalid_alignment = axk::align_up(10, 0);
  ASSERT_FALSE(invalid_alignment);
  EXPECT_EQ(invalid_alignment.error().code, axk::ErrorCode::invalid_argument);
}

TEST(ByteReader, ReadsEndianAndSignedValues) {
  const std::array bytes{std::byte{0x80}, std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
                         std::byte{0x78}};
  const axk::ByteReader reader{bytes};

  EXPECT_EQ(reader.u8(0), 0x80U);
  EXPECT_EQ(reader.s8(0), static_cast<std::int8_t>(-128));
  EXPECT_EQ(reader.be16(1), 0x1234U);
  EXPECT_EQ(reader.be32(1), 0x12345678U);
  EXPECT_EQ(reader.le16(1), 0x3412U);
  EXPECT_EQ(reader.le32(1), 0x78563412U);
  EXPECT_EQ(reader.sbe16(0), static_cast<std::int16_t>(-32750));
  EXPECT_EQ(reader.sbe32(0), static_cast<std::int32_t>(0x80123456U));
  EXPECT_EQ(reader.sle16(0), static_cast<std::int16_t>(0x1280));
  EXPECT_EQ(reader.sle32(0), static_cast<std::int32_t>(0x56341280U));
}

TEST(Error, RendersSamplerContextBeforeOptionalTechnicalTrace) {
  axk::ErrorContext context;
  context.partition_index = 3;
  context.volume_name = "Strings";
  context.object_type = "SBNK";
  context.object_name = "Stereo Pad";
  context.source_path = "disk.hds";
  context.raw_offset = 4096;
  const auto error = axk::make_error(axk::ErrorCode::object_malformed, axk::ErrorCategory::object,
                                     "sample bank is malformed", context);

  EXPECT_EQ(axk::render_error(error, false),
            "object[400]: sample bank is malformed [partition=3, volume=Strings, "
            "object_type=SBNK, object=Stereo Pad]");
  EXPECT_NE(axk::render_error(error).find("source=disk.hds"), std::string::npos);
  EXPECT_NE(axk::render_error(error).find("offset=4096"), std::string::npos);
}

TEST(ByteReader, RejectsEveryTruncatedReadWithOffsetContext) {
  const std::array bytes{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  const axk::ByteReader reader{bytes};

  const std::array<std::size_t, 3> invalid_offsets{3U, 4U, std::numeric_limits<std::size_t>::max()};
  for (const auto offset : invalid_offsets) {
    const auto result = reader.slice(offset, 1);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::out_of_bounds);
    ASSERT_TRUE(result.error().context.raw_offset.has_value());
    EXPECT_EQ(*result.error().context.raw_offset, offset);
  }
  EXPECT_FALSE(reader.be32(0));
}

TEST(ByteReader, CleansAsciiAndCanRejectInvalidBytes) {
  const std::array bytes{std::byte{'N'}, std::byte{'a'}, std::byte{'m'},  std::byte{'e'},
                         std::byte{' '}, std::byte{' '}, std::byte{0x00}, std::byte{0xff}};
  const axk::ByteReader reader{bytes};

  EXPECT_EQ(reader.ascii_field(0, 8), std::string{"Name"});
  const auto strict = reader.ascii_field(7, 1, false);
  ASSERT_FALSE(strict);
  EXPECT_EQ(strict.error().code, axk::ErrorCode::invalid_ascii);
  EXPECT_EQ(reader.ascii_field(7, 1, true), std::string{"?"});
  EXPECT_EQ(reader.printable_ascii_field(3, 5), std::string{"e  ??"});
  const std::string decoded_expected{"\0\xef\xbf\xbd", 4};
  EXPECT_EQ(reader.decoded_ascii_field(6, 2), decoded_expected);
}

TEST(ByteWriter, WritesWithoutResizingFixedStorage) {
  std::array<std::byte, 16> bytes{};
  axk::ByteWriter writer{bytes};

  ASSERT_TRUE(writer.write_be16(0, 0x1234));
  ASSERT_TRUE(writer.write_be32(2, 0x56789abc));
  ASSERT_TRUE(writer.write_le16(6, 0x1234));
  ASSERT_TRUE(writer.write_le32(8, 0x56789abc));
  ASSERT_TRUE(writer.write_ascii_field(12, 4, "AXK"));

  const axk::ByteReader reader{bytes};
  EXPECT_EQ(reader.be16(0), 0x1234U);
  EXPECT_EQ(reader.be32(2), 0x56789abcU);
  EXPECT_EQ(reader.le16(6), 0x1234U);
  EXPECT_EQ(reader.le32(8), 0x56789abcU);
  EXPECT_EQ(reader.ascii_field(12, 4), std::string{"AXK"});
  EXPECT_EQ(writer.size(), 16U);

  EXPECT_FALSE(writer.write_be32(14, 1));
  EXPECT_FALSE(writer.write_ascii_field(0, 2, "long"));
  EXPECT_FALSE(writer.write_ascii_field(0, 4, "\xc3\xa4"));
}
