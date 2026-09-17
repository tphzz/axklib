#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

constexpr std::array models{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};

axk::DecodedSystemFile retained_file(axk::ASeriesModel model, std::uint8_t revision = 0) {
    const bool native = model == axk::ASeriesModel::a3000;
    const auto size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + size);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>((i * 73U + 19U) & 255U);
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x36));
    EXPECT_TRUE(writer.write_be32(0x1c, size + 8U));
    EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
    bytes[0x3e] = static_cast<std::byte>(revision);
    return axk::decode_system_file(
               native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes)
        .value();
}

} // namespace

TEST(SystemFavorites, ReadsRawSelectionsAndEligibilityWithoutNormalizing) {
    for (const auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto file = retained_file(model);
        const auto before = axk::encode_system_file(file).value();
        const auto result = axk::decode_system_favorites(file);
        ASSERT_TRUE(result);
        ASSERT_EQ(result->effects.size(), native ? 55U : 97U);
        ASSERT_EQ(result->raw_bytes.size(), native ? 128U : 256U);
        const auto start = native ? 0xa0U : 0x230U;
        const auto raw = std::span{before}.subspan(start, result->raw_bytes.size());
        EXPECT_EQ(result->raw_bytes, (std::vector<std::byte>{raw.begin(), raw.end()}));
        const auto &through = result->effects[0];
        EXPECT_EQ(through.raw_type, 0U);
        EXPECT_EQ(through.parameter_count, 0U);
        EXPECT_EQ(through.editable_position_count, 0U);
        const auto &cancel = result->effects[15];
        EXPECT_EQ(cancel.parameter_count, 2U);
        EXPECT_EQ(cancel.editable_position_count, native ? 2U : 4U);
        const auto &effect = result->effects[7];
        EXPECT_EQ(effect.raw_type, 7U);
        const auto offset = start + (native ? 7U : 5U) * 2U;
        const auto first = std::to_integer<unsigned>(before[offset]);
        const auto second = std::to_integer<unsigned>(before[offset + 1U]);
        EXPECT_EQ(effect.selections, (std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(first >> 4U),
                                                                  static_cast<std::uint8_t>(first & 15U),
                                                                  static_cast<std::uint8_t>(second >> 4U),
                                                                  static_cast<std::uint8_t>(second & 15U)}));
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
}

TEST(SystemFavorites, PatchesOnlyRequestedNibblesAcrossEveryStoredByteValue) {
    for (const auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto original = retained_file(model);
        const auto offset = (native ? 0xa0U + 5U * 2U : 0x230U + 56U * 2U);
        for (unsigned byte = 0; byte < 256U; ++byte) {
            auto before = axk::encode_system_file(original).value();
            before[offset] = static_cast<std::byte>(byte);
            before[offset + 1U] = static_cast<std::byte>(255U - byte);
            const auto file = axk::decode_system_file(original.kind, before).value();
            for (std::size_t position = 0; position < 4U; ++position) {
                axk::SystemEffectFavoritesPatch patch;
                patch.raw_type = 5;
                patch.selections[position] = 15;
                const auto result = axk::patch_system_favorites(file, std::array{patch}, model);
                ASSERT_TRUE(result);
                auto expected = before;
                expected[offset + position / 2U] |= position % 2U == 0U ? std::byte{0xf0} : std::byte{0x0f};
                EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
            }
            EXPECT_EQ(axk::encode_system_file(file).value(), before);
        }
    }
}

TEST(SystemFavorites, RejectsUnavailableRequestsAndDuplicateRowsButAllowsDuplicateSelections) {
    for (const auto model : models) {
        const auto file = retained_file(model);
        const auto before = file;
        EXPECT_EQ(axk::patch_system_favorites(file, {}, model).value(), file);
        axk::SystemEffectFavoritesPatch patch;
        patch.raw_type = 15;
        patch.selections = {1, 1, {}, {}};
        ASSERT_TRUE(axk::patch_system_favorites(file, std::array{patch}, model));
        EXPECT_FALSE(axk::patch_system_favorites(file, std::array{patch, patch}, model));
        patch.selections[2] = 1;
        EXPECT_EQ(axk::patch_system_favorites(file, std::array{patch}, model).has_value(),
                  model != axk::ASeriesModel::a3000);
        patch.selections = {2, {}, {}, {}};
        EXPECT_FALSE(axk::patch_system_favorites(file, std::array{patch}, model));
        patch.raw_type = 0;
        patch.selections[0] = 0;
        EXPECT_FALSE(axk::patch_system_favorites(file, std::array{patch}, model));
        patch.raw_type = model == axk::ASeriesModel::a3000 ? 55 : 97;
        EXPECT_FALSE(axk::patch_system_favorites(file, std::array{patch}, model));
        patch.raw_type = 5;
        patch.selections[0] = 16;
        EXPECT_FALSE(axk::patch_system_favorites(file, std::array{patch}, model));
        EXPECT_EQ(file, before);
    }
}

TEST(SystemFavorites, EveryOrdinaryRowUsesItsGenerationOrderAndExactVisibleBoundary) {
    constexpr std::array<unsigned, 97> rows{
        0,  1,  2,  3,  4,  56, 57, 5,  10, 11, 12, 13, 14, 6,  7,  29, 26, 28, 19, 20, 21, 22, 38, 39, 40,
        30, 31, 36, 46, 48, 52, 49, 53, 54, 8,  9,  50, 58, 59, 55, 67, 75, 69, 70, 71, 72, 68, 80, 81, 82,
        83, 84, 85, 86, 87, 76, 77, 78, 79, 25, 15, 23, 24, 32, 33, 44, 45, 37, 51, 73, 74, 60, 41, 42, 43,
        61, 62, 63, 64, 65, 66, 34, 35, 16, 17, 18, 47, 27, 88, 89, 90, 91, 92, 93, 94, 95, 96};
    constexpr std::array<unsigned, 97> counts{
        0,  11, 16, 14, 9,  16, 16, 14, 7,  13, 16, 7,  11, 9,  9,  2,  8,  3,  14, 14, 13, 13, 10, 10, 6,
        5,  14, 4,  13, 13, 13, 11, 11, 11, 10, 10, 8,  10, 10, 12, 11, 7,  12, 11, 13, 10, 9,  11, 11, 11,
        11, 15, 15, 15, 15, 5,  11, 11, 11, 4,  10, 12, 12, 12, 12, 10, 10, 11, 16, 10, 10, 15, 10, 10, 6,
        10, 10, 11, 14, 14, 15, 5,  7,  5,  5,  4,  13, 7,  10, 10, 11, 15, 13, 11, 15, 16, 16};
    for (const auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto file = retained_file(model, native ? 0 : 1);
        const auto before = axk::encode_system_file(file).value();
        const auto decoded = axk::decode_system_favorites(file).value();
        for (std::uint16_t raw = 0; raw < (native ? 55U : 97U); ++raw) {
            SCOPED_TRACE(raw);
            EXPECT_EQ(decoded.effects[raw].parameter_count, counts[raw]);
            const auto offset = native ? 0xa0U + raw * 2U : 0x230U + rows[raw] * 2U;
            for (std::size_t position = 0; position < 4U; ++position) {
                const auto byte = std::to_integer<unsigned>(before[offset + position / 2U]);
                const auto shift = position % 2U == 0U ? 4U : 0U;
                EXPECT_EQ(decoded.effects[raw].selections[position], (byte >> shift) & 15U);
                axk::SystemEffectFavoritesPatch patch;
                patch.raw_type = raw;
                patch.selections[position] = static_cast<std::uint8_t>(counts[raw] == 0U ? 0U : counts[raw] - 1U);
                const auto result = axk::patch_system_favorites(file, std::array{patch}, model);
                const bool eligible = counts[raw] != 0U && (!native || position < counts[raw]);
                ASSERT_EQ(result.has_value(), eligible);
                if (result) {
                    auto expected = before;
                    expected[offset + position / 2U] =
                        static_cast<std::byte>((byte & ~(15U << shift)) | ((counts[raw] - 1U) << shift));
                    EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                }
                patch.selections[position] = static_cast<std::uint8_t>(counts[raw]);
                EXPECT_FALSE(axk::patch_system_favorites(file, std::array{patch}, model));
            }
        }
    }
}

TEST(SystemFavorites, RequiresMatchingModelAndValidRetainedLayout) {
    const auto native = retained_file(axk::ASeriesModel::a3000);
    const auto current = retained_file(axk::ASeriesModel::a4000, 1);
    ASSERT_TRUE(axk::decode_system_favorites(current));
    EXPECT_FALSE(axk::patch_system_favorites(native, {}, axk::ASeriesModel::a4000));
    EXPECT_FALSE(axk::patch_system_favorites(current, {}, axk::ASeriesModel::a3000));
    EXPECT_FALSE(axk::patch_system_favorites(current, {}, static_cast<axk::ASeriesModel>(255)));
    auto invalid = native;
    invalid.system_bulk_bytes.pop_back();
    EXPECT_FALSE(axk::decode_system_favorites(invalid));
    EXPECT_FALSE(axk::patch_system_favorites(invalid, {}, axk::ASeriesModel::a3000));
    invalid = current;
    invalid.storage_revision = 2;
    EXPECT_FALSE(axk::decode_system_favorites(invalid));
    invalid = current;
    invalid.system_header_bytes.clear();
    EXPECT_FALSE(axk::patch_system_favorites(invalid, {}, axk::ASeriesModel::a4000));
    invalid = current;
    invalid.kind = static_cast<axk::SystemFileKind>(255);
    EXPECT_FALSE(axk::decode_system_favorites(invalid));
}
