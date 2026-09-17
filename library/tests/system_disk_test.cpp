#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile retained_disk(bool native, std::uint8_t revision, unsigned value = 0xa5U) {
    const auto size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + size, static_cast<std::byte>(value));
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

constexpr auto models = std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};

void write_mask(std::vector<std::byte> &bytes, bool native, std::uint32_t mask) {
    axk::ByteWriter writer{bytes};
    if (native)
        EXPECT_TRUE(writer.write_u8(0x91, static_cast<std::uint8_t>(mask)));
    else
        EXPECT_TRUE(writer.write_be32(0x224, mask));
}

} // namespace

TEST(SystemDisk, ReadsAllStoredValuesWithoutNormalizationAndRetainsSeedsAndTheCompleteBlock) {
    for (bool native : {false, true}) {
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            for (unsigned value = 0; value < 256U; ++value) {
                const auto file = retained_disk(native, revision, value);
                const auto before = axk::encode_system_file(file).value();
                const auto decoded = axk::decode_system_disk(file);
                ASSERT_TRUE(decoded);
                EXPECT_EQ(decoded->raw_bytes,
                          std::vector<std::byte>(native ? 20U : 16U, static_cast<std::byte>(value)));
                EXPECT_EQ(decoded->seed_accumulator, value * 0x01010101U);
                const auto &p = decoded->parameters;
                EXPECT_EQ(p.scsi_id.has_value(), value <= 7U);
                if (p.scsi_id)
                    EXPECT_EQ(*p.scsi_id, value);
                EXPECT_EQ(p.top_partition.has_value(), value <= 98U);
                if (p.top_partition)
                    EXPECT_EQ(*p.top_partition, value + 1U);
                for (std::size_t i = 0; i < p.scsi_mounts.size(); ++i)
                    EXPECT_EQ(p.scsi_mounts[i], (value & (1U << i)) != 0U);
                for (std::size_t i = 0; i < p.ide_mounts.size(); ++i) {
                    EXPECT_EQ(p.ide_mounts[i].has_value(), !native);
                    if (!native)
                        EXPECT_EQ(*p.ide_mounts[i], (value & (1U << i)) != 0U);
                }
                EXPECT_EQ(axk::encode_system_file(file).value(), before);
            }
        }
    }
}

TEST(SystemDisk, ReadsAsymmetricBigEndianStateAndPreservesUnknownUpperMountBits) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto source = retained_disk(native, revision);
            auto bytes = axk::encode_system_file(source).value();
            const std::size_t offset = native ? 0x90U : 0x220U;
            bytes[offset] = std::byte{6};
            axk::ByteWriter writer{bytes};
            ASSERT_TRUE(writer.write_be32(offset + (native ? 4U : 8U), 0x12345678U));
            write_mask(bytes, native, native ? 0x85U : 0xc0010205U);
            const auto file = axk::decode_system_file(source.kind, bytes);
            ASSERT_TRUE(file);
            const auto decoded = axk::decode_system_disk(*file);
            ASSERT_TRUE(decoded);
            EXPECT_EQ(decoded->seed_accumulator, 0x12345678U);
            for (std::size_t i = 0; i < 8U; ++i)
                EXPECT_EQ(decoded->parameters.scsi_mounts[i], i == 0U || i == 2U || (native && i == 7U));
            if (!native) {
                EXPECT_EQ(decoded->parameters.ide_mounts[0], false);
                EXPECT_EQ(decoded->parameters.ide_mounts[1], true);
            }
            axk::SystemDiskParameters patch;
            patch.scsi_mounts[0] = false;
            if (!native)
                patch.ide_mounts[0] = true;
            auto expected = bytes;
            write_mask(expected, native, native ? 0x84U : 0xc0010304U);
            const auto updated = axk::patch_system_disk(*file, patch, model);
            ASSERT_TRUE(updated);
            EXPECT_EQ(axk::encode_system_file(*updated).value(), expected);
            EXPECT_EQ(axk::decode_system_disk(*updated)->seed_accumulator, 0x12345678U);
        }
    }
}

TEST(SystemDisk, ValidatesEveryIdAndTopPartitionRequestAndChangesOnlyTheirDependentBytes) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_disk(native, revision);
            const auto before = axk::encode_system_file(file).value();
            EXPECT_EQ(axk::patch_system_disk(file, {}, model).value(), file);
            for (unsigned value = 0; value < 256U; ++value) {
                for (bool top : {false, true}) {
                    axk::SystemDiskParameters patch;
                    (top ? patch.top_partition : patch.scsi_id) = static_cast<std::uint8_t>(value);
                    const auto result = axk::patch_system_disk(file, patch, model);
                    ASSERT_EQ(result.has_value(), top ? value >= 1U && value <= 98U : value <= 7U);
                    if (result) {
                        auto expected = before;
                        if (top) {
                            expected[native ? 0x98U : 0x22cU] = static_cast<std::byte>(value - 1U);
                        } else {
                            expected[native ? 0x90U : 0x220U] = static_cast<std::byte>(value);
                            write_mask(expected, native, (native ? 0xa5U : 0xa5a5a5a5U) & ~(1U << value));
                        }
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        EXPECT_EQ(axk::patch_system_disk(*result, patch, model).value(), *result);
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            }
        }
    }
}

TEST(SystemDisk, EditsIndividualMountBitsAndClearsOnlyTheLoadEffectiveOwnIdBit) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_disk(native, revision);
            const auto before = axk::encode_system_file(file).value();
            for (std::size_t i = 0; i < 10U; ++i) {
                for (bool value : {false, true}) {
                    axk::SystemDiskParameters patch;
                    if (i < 8U)
                        patch.scsi_mounts[i] = value;
                    else
                        patch.ide_mounts[i - 8U] = value;
                    const auto result = axk::patch_system_disk(file, patch, model);
                    ASSERT_EQ(result.has_value(), !(native && i >= 8U) && !(i == 5U && value));
                    if (result) {
                        auto expected = before;
                        auto mask = native ? 0xa5U : 0xa5a5a5a5U;
                        mask = value ? mask | (1U << i) : mask & ~(1U << i);
                        write_mask(expected, native, mask & ~(1U << 5U));
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            }
        }
    }
}

TEST(SystemDisk, UsesFinalRequestedIdAndPreservesNativeFavoritesInCombinedPatches) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto file = retained_disk(native, native ? 0 : 1);
        const auto before = axk::encode_system_file(file).value();
        axk::SystemFilePatch patch;
        patch.disk.scsi_id = 6;
        patch.disk.scsi_mounts[5] = true;
        patch.disk.scsi_mounts[6] = false;
        patch.disk.top_partition = 98;
        patch.favorites.push_back({1, {std::uint8_t{2}, std::nullopt}});
        const auto favorites = axk::patch_system_favorites(file, patch.favorites, model);
        ASSERT_TRUE(favorites);
        auto expected = axk::encode_system_file(*favorites).value();
        expected[native ? 0x90U : 0x220U] = std::byte{6};
        expected[native ? 0x98U : 0x22cU] = std::byte{97};
        write_mask(expected, native, ((native ? 0xa5U : 0xa5a5a5a5U) | 0x20U) & ~0x40U);
        const auto result = axk::patch_system_file(file, patch, model);
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
        patch.disk.scsi_mounts[6] = true;
        EXPECT_FALSE(axk::patch_system_file(file, patch, model));
        patch.disk.scsi_mounts[6] = false;
        patch.disk.top_partition = 99;
        EXPECT_FALSE(axk::patch_system_file(file, patch, model));
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
}

TEST(SystemDisk, RejectsMalformedRecordsAndMismatchedModelsEvenForEmptyPatches) {
    const auto file = retained_disk(false, 1);
    for (const auto model : {axk::ASeriesModel::a3000, static_cast<axk::ASeriesModel>(255)})
        EXPECT_FALSE(axk::patch_system_disk(file, {}, model));
    EXPECT_FALSE(axk::patch_system_disk(retained_disk(true, 0), {}, axk::ASeriesModel::a5000));
    for (unsigned defect = 0; defect < 5U; ++defect) {
        auto malformed = file;
        if (defect == 0U)
            malformed.system_bulk_bytes.pop_back();
        else if (defect == 1U)
            malformed.storage_revision = 2;
        else if (defect == 2U)
            malformed.system_header_bytes[0x0e] = std::byte{};
        else if (defect == 3U)
            malformed.kind = static_cast<axk::SystemFileKind>(255);
        else
            malformed.system_header_bytes.clear();
        EXPECT_FALSE(axk::decode_system_disk(malformed));
        EXPECT_FALSE(axk::patch_system_disk(malformed, {}, axk::ASeriesModel::a5000));
    }
}
