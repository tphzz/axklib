#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_slot_sectors = 0x1fffffU;

template <std::size_t Size> std::array<std::byte, Size> read_raw_at(std::ifstream &file, std::uint64_t offset) {
    std::array<std::byte, Size> bytes{};
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    EXPECT_EQ(file.gcount(), static_cast<std::streamsize>(bytes.size())) << offset;
    return bytes;
}

std::uint32_t raw_be32(std::span<const std::byte> bytes, std::size_t offset) {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[offset + 3U]);
}

axk::HdsBuildManifest empty_manifest(std::uint64_t size_bytes, std::uint8_t partition_count) {
    axk::HdsBuildManifest manifest{"1.0", size_bytes, {}};
    for (std::uint8_t index = 0; index < partition_count; ++index)
        manifest.partitions.push_back({"P" + std::to_string(index + 1U), {}});
    return manifest;
}

class HdsCapacity : public testing::Test {
  protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() / "axklib-hds-capacity-test";
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
};

TEST(HdsCapacityGeometry, PublishesEightGiBBoundaryAndRejectsInvalidSizes) {
    EXPECT_EQ(axk::maximum_hds_size, 8U * gib);
    for (const auto size : {8U * gib - 512U, 8U * gib}) {
        const auto geometry = axk::plan_hds_geometry(empty_manifest(size, 8U));
        ASSERT_TRUE(geometry) << geometry.error().message;
    }
    for (const auto size : {8U * gib - 1U, 8U * gib + 1U, 8U * gib + 512U})
        EXPECT_FALSE(axk::plan_hds_geometry(empty_manifest(size, 8U))) << size;
}

TEST(HdsCapacityGeometry, PreservesExplicitTwoGiBOnePartitionCapAndUnusedTail) {
    const auto geometry = axk::plan_hds_geometry(empty_manifest(2U * gib, 1U));
    ASSERT_TRUE(geometry) << geometry.error().message;
    ASSERT_EQ(geometry->size(), 1U);
    const auto &partition = geometry->front();
    EXPECT_EQ(partition.start_sector, 3U);
    EXPECT_EQ(partition.slot_sector_count, maximum_slot_sectors);
    EXPECT_EQ(partition.filesystem_sector_count, maximum_slot_sectors - 1U);
    EXPECT_EQ(2U * gib / 512U - partition.start_sector - partition.filesystem_sector_count, maximum_slot_sectors);
}

TEST(HdsCapacityGeometry, KeepsCappedEqualSlotsAcrossLargeSizesAndEveryPartitionCount) {
    for (const auto size : {4U * gib, 8U * gib - 512U, 8U * gib}) {
        for (std::uint8_t count = 1U; count <= 8U; ++count) {
            SCOPED_TRACE(std::to_string(size) + "/" + std::to_string(count));
            const auto geometry = axk::plan_hds_geometry(empty_manifest(size, count));
            ASSERT_TRUE(geometry) << geometry.error().message;
            ASSERT_EQ(geometry->size(), count);
            const auto slot = std::min((size / 512U - 2U) / count, maximum_slot_sectors);
            for (std::size_t index = 0; index < geometry->size(); ++index) {
                const auto &partition = (*geometry)[index];
                EXPECT_EQ(partition.start_sector, 3U + index * slot);
                EXPECT_EQ(partition.slot_sector_count, slot);
                EXPECT_EQ(partition.filesystem_sector_count, slot - 1U);
                EXPECT_LE(partition.start_sector + partition.filesystem_sector_count, size / 512U);
            }
        }
    }
}

TEST(HdsCapacityProfiles, AcceptsOnlyTheAdmittedCountsForNewQuickProfiles) {
    const std::array cases{
        std::tuple{"hds-128-mib", 128ULL * 1024ULL * 1024ULL, 1U},
        std::tuple{"hds-256-mib", 256ULL * 1024ULL * 1024ULL, 1U},
        std::tuple{"hds-4-gib", 4ULL * gib, 4U},
        std::tuple{"hds-8-gib", 8ULL * gib, 8U},
    };
    for (const auto &[id, size, minimum_count] : cases) {
        SCOPED_TRACE(id);
        const auto profile = axk::parse_hds_creation_profile_id(id);
        ASSERT_TRUE(profile) << profile.error().message;
        for (std::uint8_t count = 0U; count <= 9U; ++count) {
            const auto plan = axk::plan_hds_creation({*profile, count});
            if (count < minimum_count || count > 8U) {
                EXPECT_FALSE(plan) << static_cast<unsigned>(count);
                continue;
            }
            ASSERT_TRUE(plan) << plan.error().message;
            EXPECT_EQ(plan->manifest.size_bytes, size);
            EXPECT_EQ(plan->manifest.partitions.size(), count);
            EXPECT_EQ(plan->summary.partitions.size(), count);
            EXPECT_LT(plan->unused_tail_sectors, static_cast<std::uint64_t>(count));
        }
    }
}

TEST_F(HdsCapacity, SparseEightGiBImageRoundTripsTinyWaveformAboveFourGiB) {
    axk::Waveform source;
    source.format = {1U, 2U, 44'100U};
    source.frame_count = 3U;
    source.pcm = {std::byte{}, std::byte{}, std::byte{0xe8}, std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
    const auto audio_path = root / "tiny.wav";
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));
    auto manifest = empty_manifest(8U * gib, 8U);
    axk::VolumeSpec volume;
    volume.name = "High Offset";
    volume.waveforms.push_back({"tiny", "Tiny", audio_path, 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Tiny Sample";
    sample.waveform_id = "tiny";
    volume.samples.push_back(std::move(sample));
    manifest.partitions.back().volumes.push_back(std::move(volume));
    const auto path = root / "eight-gib.hds";

    const auto written = axk::write_hds_image(manifest, path);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(std::filesystem::file_size(path), 8U * gib);
    EXPECT_EQ(written->unused_tail_sectors, 6U);
    {
        std::ifstream raw{path, std::ios::binary};
        ASSERT_TRUE(raw);
        const auto superblock = read_raw_at<512U>(raw, 0U);
        EXPECT_EQ(superblock, read_raw_at<512U>(raw, 512U));
        EXPECT_EQ(raw_be32(superblock, 0x9cU), 512U);
        EXPECT_EQ(raw_be32(superblock, 0xa0U), 0x01000000U);

        // Partition 4 starts one sector below 4 GiB; its header spans the boundary.
        const std::array high_offsets{
            std::tuple{4U, 4U * gib - 512U, 0x007fffffU},
            std::tuple{5U, 5U * gib - 1024U, 0x009ffffeU},
            std::tuple{6U, 6U * gib - 1536U, 0x00bffffdU},
            std::tuple{7U, 7U * gib - 2048U, 0x00dffffcU},
        };
        for (const auto &[index, offset, start_sector] : high_offsets) {
            SCOPED_TRACE(index);
            EXPECT_EQ(raw_be32(superblock, 0xa8U + index * 8U), start_sector);
            EXPECT_EQ(raw_be32(superblock, 0xacU + index * 8U), 0x001ffffeU);
            const auto primary = read_raw_at<1024U>(raw, offset);
            const auto backup = read_raw_at<1024U>(raw, offset + 1024U);
            EXPECT_EQ(primary, backup);
            for (const auto &header : {primary, backup}) {
                EXPECT_EQ(std::string(reinterpret_cast<const char *>(header.data()), 11U), "YAMAHA_dev3");
                EXPECT_EQ(raw_be32(header, 0x80U), 2U);
                EXPECT_EQ(raw_be32(header, 0x104U), start_sector);
                EXPECT_EQ(raw_be32(header, 0x118U), start_sector);
                EXPECT_EQ(raw_be32(header, 0x11cU), 0x001ffffeU);
                EXPECT_EQ(raw_be32(header, 0x178U), start_sector);
                EXPECT_EQ(raw_be32(header, 0x17cU), 0x001ffffeU);
            }
        }

        // The fixed fixture puts SMPL record 9 in the first directory-index page.
        constexpr auto last_partition_offset = 7U * gib - 2048U;
        constexpr auto index_offset = last_partition_offset + 0x102U * 1024U;
        const auto wave_record = read_raw_at<72U>(raw, index_offset + 9U * 72U);
        EXPECT_EQ(raw_be32(wave_record, 0x06U), 0x20cU);
        EXPECT_EQ(raw_be32(wave_record, 0x0aU), 0x296U);
        constexpr auto wave_offset = last_partition_offset + 0x296U * 1024U;
        const auto wave_payload = read_raw_at<0x20cU>(raw, wave_offset);
        EXPECT_EQ(std::string(reinterpret_cast<const char *>(wave_payload.data()), 12U), "FSFSDEV3SPLX");
        const std::array expected_pcm{
            std::byte{0x00}, std::byte{0x00}, std::byte{0x03}, std::byte{0xe8}, std::byte{0xfc}, std::byte{0x18},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x03}, std::byte{0xe8}, std::byte{0xfc}, std::byte{0x18},
        };
        EXPECT_TRUE(std::ranges::equal(std::span{wave_payload}.subspan(0x200U), expected_pcm));
    }
    const auto reopened = axk::open_image(path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened->partitions().size(), 8U);
    EXPECT_EQ(reopened->image_size_bytes(), 8U * gib);
    EXPECT_TRUE(reopened->backup_superblock_matches());
    const auto &partition = reopened->partitions().back();
    EXPECT_EQ(partition.start_sector, 3U + 7U * maximum_slot_sectors);
    EXPECT_GT(static_cast<std::uint64_t>(partition.start_sector) * 512U, 4U * gib);
    EXPECT_TRUE(partition.backup_header_matches);
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    ASSERT_EQ(catalog->objects.size(), 2U);
    const auto wave = std::ranges::find_if(
        catalog->objects, [](const auto &item) { return item.object.header.type == axk::ObjectType::smpl; });
    ASSERT_NE(wave, catalog->objects.end());
    EXPECT_EQ(wave->partition, axk::PartitionIndex{7U});
    const auto record = std::ranges::find(partition.records, wave->sfs_id, &axk::IndexRecord::sfs_id);
    ASSERT_NE(record, partition.records.end());
    EXPECT_GT(record->record_offset.value, 4U * gib);
    const auto decoded = axk::decode_waveform(*reopened, *wave);
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_GE(decoded->pcm.size(), source.pcm.size());
    EXPECT_TRUE(std::ranges::equal(source.pcm, std::span<const std::byte>{decoded->pcm}.first(source.pcm.size())));
}

} // namespace
