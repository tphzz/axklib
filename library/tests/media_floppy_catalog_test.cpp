#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/writer_internal.hpp"

namespace {

constexpr std::size_t record_bytes = 38U;

std::string record_text(std::span<const std::byte> bytes, std::size_t record) {
    const auto payload = bytes.subspan(record * record_bytes + 1U, record_bytes - 1U);
    const auto end = std::ranges::find(payload, std::byte{});
    return {reinterpret_cast<const char *>(payload.data()), static_cast<std::size_t>(end - payload.begin())};
}

} // namespace

TEST(YamahaFloppyCatalog, EncodesTheConfirmedFixedRecordContract) {
    const std::vector files{
        axk::YamahaFloppyCatalogEntry{2U, R"(\PROG\001             )"},
        axk::YamahaFloppyCatalogEntry{3U, R"(\SMPL\LONG WAVE       01)"},
        axk::YamahaFloppyCatalogEntry{79U, R"(\A3000F.SYM)"},
    };
    const std::vector<std::string> categories{R"(\OTHERS)", R"(\PROG)", R"(\SMPL)"};

    const auto encoded = axk::detail::encode_yamaha_floppy_catalog("MF CATALOG    01", files, categories);

    ASSERT_TRUE(encoded) << encoded.error().message;
    ASSERT_EQ(encoded->size(), 257U * record_bytes);
    EXPECT_EQ((*encoded)[0], std::byte{});
    EXPECT_EQ(record_text(*encoded, 0U), "MF CATALOG    01");
    EXPECT_EQ((*encoded)[record_bytes], std::byte{0xff});
    EXPECT_EQ((*encoded)[2U * record_bytes], std::byte{0xff});
    EXPECT_EQ((*encoded)[3U * record_bytes], std::byte{});
    EXPECT_EQ(record_text(*encoded, 3U), R"(\PROG\001             )");
    EXPECT_EQ(record_text(*encoded, 4U), R"(\SMPL\LONG WAVE       01)");
    EXPECT_EQ(record_text(*encoded, 80U), R"(\A3000F.SYM)");
    EXPECT_EQ(record_text(*encoded, 225U), R"(\OTHERS)");
    EXPECT_EQ(record_text(*encoded, 226U), R"(\PROG)");
    EXPECT_EQ(record_text(*encoded, 227U), R"(\SMPL)");
    EXPECT_TRUE(std::ranges::all_of(std::span{*encoded}.subspan(record_bytes + 1U, record_bytes - 1U),
                                    [](std::byte value) { return value == std::byte{}; }));

    const auto decoded = axk::detail::decode_yamaha_floppy_catalog(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->disk_name, "MF CATALOG    01");
    EXPECT_EQ(decoded->files, files);
    EXPECT_EQ(decoded->categories, categories);
}

TEST(YamahaFloppyCatalog, BuildsLogicalPathsAndSlotBasedPhysicalNames) {
    const auto program = axk::detail::yamaha_floppy_object_path(axk::ObjectType::prog, "001");
    const auto wave = axk::detail::yamaha_floppy_object_path(axk::ObjectType::smpl, "LONG WAVE", 1U);
    const auto object_filename = axk::detail::yamaha_floppy_physical_filename("Source Wave", 2U);
    const auto marker_filename = axk::detail::yamaha_floppy_physical_filename("A3000F.SYM", 79U);

    ASSERT_TRUE(program);
    ASSERT_TRUE(wave);
    ASSERT_TRUE(object_filename);
    ASSERT_TRUE(marker_filename);
    EXPECT_EQ(*program, R"(\PROG\001             )");
    EXPECT_EQ(*wave, R"(\SMPL\LONG WAVE       01)");
    EXPECT_EQ(*object_filename, "SOURCEWA.002");
    EXPECT_EQ(*marker_filename, "A3000F_S.079");
}

TEST(YamahaFloppyCatalog, RejectsDuplicateSlotsAndOversizedFields) {
    const std::vector duplicate_files{
        axk::YamahaFloppyCatalogEntry{2U, R"(\PROG\001             )"},
        axk::YamahaFloppyCatalogEntry{2U, R"(\SMPL\WAVE            )"},
    };
    const std::vector<std::string> categories{R"(\OTHERS)"};

    EXPECT_FALSE(axk::detail::encode_yamaha_floppy_catalog("DISK", duplicate_files, categories));
    EXPECT_FALSE(axk::detail::encode_yamaha_floppy_catalog(std::string(37U, 'D'), {}, categories));
    EXPECT_FALSE(axk::detail::yamaha_floppy_object_path(axk::ObjectType::unknown, "UNKNOWN"));
    EXPECT_FALSE(axk::detail::yamaha_floppy_physical_filename("OBJECT", 224U));
}
