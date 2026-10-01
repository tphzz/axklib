#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/capacity_admission_internal.hpp"
#include "axklib/filesystem_edit.hpp"
#include "axklib/filesystem_transaction.hpp"
#include "axklib/io.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sequence.hpp"
#include "axklib/sfs.hpp"
#include "axklib/sfs_allocation.hpp"
#include "axklib/volume_capacity.hpp"
#include "axklib/writer.hpp"

namespace {

class MetadataReader final : public axk::RandomAccessReader {
  public:
    explicit MetadataReader(std::shared_ptr<const axk::RandomAccessReader> source) : source_(std::move(source)) {}
    std::uint64_t size() const noexcept override { return source_->size(); }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        if (armed && offset < pcm_end && offset + bytes.size() > pcm_begin)
            return std::unexpected{axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                                                   "Capacity inspection attempted to read PCM")};
        if (armed)
            largest_read = std::max(largest_read, bytes.size());
        return source_->read_exact_at(offset, bytes);
    }
    bool armed{};
    std::uint64_t pcm_begin{}, pcm_end{};
    mutable std::size_t largest_read{};

  private:
    std::shared_ptr<const axk::RandomAccessReader> source_;
};

class VolumeCapacityScan : public testing::Test {
  protected:
    std::filesystem::path folder, source;
    axk::SfsId volume_id{};

    void SetUp() override {
        folder = std::filesystem::temp_directory_path() /
                 std::format("axk-capacity-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directory(folder);
        source = folder / "source.hds";
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44100U};
        waveform.frame_count = 4096U;
        waveform.pcm.resize(8192U);
        ASSERT_TRUE(axk::write_wav_atomic(folder / "wave.wav", waveform));
        axk::VolumeSpec volume;
        volume.name = "V";
        volume.waveforms.push_back({"wave", "Wave", folder / "wave.wav", 60U, {}});
        axk::SampleSpec sample;
        sample.name = "Sample";
        sample.waveform_id = "wave";
        volume.samples.push_back(sample);
        ASSERT_TRUE(axk::write_hds_image({"1.0", 8U * 1024U * 1024U, {{"P", {volume}}}}, source));
        const auto image = axk::open_image(source).value();
        const auto &partition = image.partitions().front();
        const auto root_id = axk::locate_partition_root_record(partition).value();
        const auto &root = *std::ranges::find(partition.records, root_id, &axk::IndexRecord::sfs_id);
        const auto &entry = *std::ranges::find(root.directory_entries, "V", &axk::DirectoryEntry::name);
        volume_id = std::ranges::find(partition.records, entry.target_link_id, &axk::IndexRecord::directory_id)->sfs_id;
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }

    static std::uint64_t physical_offset(const axk::Container &image, const axk::IndexRecord &record) {
        const auto &partition = image.partitions().front();
        return static_cast<std::uint64_t>(partition.start_sector) * image.superblock().sector_size_bytes +
               static_cast<std::uint64_t>(record.extents.front().cluster_offset) * partition.sectors_per_cluster *
                   image.superblock().sector_size_bytes;
    }
};

TEST_F(VolumeCapacityScan, ScansOnlyMetadataAndUsesPhysicalVolumeIdentity) {
    auto reader = std::make_shared<MetadataReader>(axk::FileReader::open(source).value());
    const auto image = axk::open_image(reader, source).value();
    const auto &records = image.partitions().front().records;
    const auto &wave = *std::ranges::find(records, "SMPL", &axk::IndexRecord::object_type);
    reader->pcm_begin = physical_offset(image, wave) + 0xacU;
    reader->pcm_end = reader->pcm_begin + 8192U;
    reader->armed = true;
    const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id);
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->volume_directory, volume_id);
    EXPECT_EQ(report->volume_name, "V");
    EXPECT_LE(reader->largest_read, 65536U);
    EXPECT_TRUE(report->profiles[1].minimum_resident_bytes);
    EXPECT_FALSE(
        axk::inspect_volume_capacity(image, {0U}, axk::locate_partition_root_record(image.partitions()[0]).value()));
}

TEST_F(VolumeCapacityScan, ShortFilesystemNamesArePaddedByTheLoaderAndRemainLoadable) {
    std::uint64_t offset{};
    std::vector<std::byte> row;
    {
        const auto image = axk::open_image(source).value();
        for (const auto &record : image.partitions().front().records) {
            const auto entry = std::ranges::find(record.directory_entries, "Wave", &axk::DirectoryEntry::name);
            if (entry != record.directory_entries.end()) {
                offset = physical_offset(image, record) + entry->payload_relative_offset;
                row = image.read_record_range({0U}, record.sfs_id, entry->payload_relative_offset, 32U).value();
            }
        }
    }
    ASSERT_NE(offset, 0U);
    {
        axk::ByteWriter writer{row};
        ASSERT_TRUE(writer.write_be16(2U, 5U));
        std::fill(row.begin() + 8, row.end(), std::byte{});
        ASSERT_TRUE(writer.write_ascii_field(8U, 4U, "Wave", std::byte{}));
        std::fstream file{source, std::ios::in | std::ios::out | std::ios::binary};
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
        ASSERT_TRUE(file);
    }
    const auto image = axk::open_image(source).value();
    const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->profiles[1].status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(report->profiles[1].resident_bytes, report->profiles[1].baseline_bytes + 336U + 72U);
}

TEST_F(VolumeCapacityScan, CancellationIsAnErrorNotAnOverrideableResult) {
    const auto image = axk::open_image(source).value();
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id, cancellation.token());
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, axk::ErrorCode::operation_cancelled);
}

TEST_F(VolumeCapacityScan, TruncatedDirectoryPrefixCannotBecomeAnEmptyFitsVolume) {
    std::uint64_t offset{};
    {
        const auto image = axk::open_image(source).value();
        for (const auto &record : image.partitions()[0].records)
            if (std::ranges::find(record.directory_entries, "Sample", &axk::DirectoryEntry::name) !=
                record.directory_entries.end())
                offset = physical_offset(image, record) + 64U;
    }
    ASSERT_NE(offset, 0U);
    {
        std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(static_cast<std::streamoff>(offset));
        const std::array<char, 2> invalid_size{};
        file.write(invalid_size.data(), invalid_size.size());
    }
    const auto image = axk::open_image(source).value();
    const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id);
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, axk::ErrorCode::object_malformed);
}

TEST_F(VolumeCapacityScan, MissingPcmExtentIsRejectedWithoutReadingPcm) {
    std::uint64_t offset{};
    {
        const auto image = axk::open_image(source).value();
        const auto &wave = *std::ranges::find(image.partitions()[0].records, "SMPL", &axk::IndexRecord::object_type);
        offset = physical_offset(image, wave) + 0x20U;
    }
    {
        std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(static_cast<std::streamoff>(offset));
        std::array<std::byte, 4> invalid_extent{};
        ASSERT_TRUE(axk::ByteWriter{invalid_extent}.write_be32(0U, 0x7fffffffU));
        file.write(reinterpret_cast<const char *>(invalid_extent.data()),
                   static_cast<std::streamsize>(invalid_extent.size()));
        ASSERT_TRUE(file);
    }
    const auto image = axk::open_image(source).value();
    const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id);
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, axk::ErrorCode::object_malformed);
}

TEST_F(VolumeCapacityScan, CompleteLaterSamplesFitBothTargetsWithoutCoverageApproval) {
    const auto source_reader = axk::FileReader::open(source).value();
    const auto image = axk::open_image(source_reader, source).value();
    const std::array destinations{axk::detail::CapacityDestination{{0U}, "V"}};
    const axk::VolumeCapacityPolicy policy{axk::ASeriesLoadTarget::a3000};
    const auto first = axk::detail::inspect_capacity_destinations(image, destinations, policy);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first->allowed);
    EXPECT_TRUE(axk::enforce_volume_capacity_admission(*first, policy));
    EXPECT_FALSE(axk::enforce_volume_capacity_admission(*first, {axk::ASeriesLoadTarget::a4000_a5000}));
    ASSERT_EQ(first->reports.size(), 1U);
    for (const auto &profile : first->reports.front().profiles)
        EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
}

TEST_F(VolumeCapacityScan, LoaderRevisionFamiliesDoNotDependOnEditableSampleProfiles) {
    std::uint64_t revision_offset{};
    {
        const auto image = axk::open_image(source).value();
        const auto &sample = *std::ranges::find(image.partitions()[0].records, "SBNK", &axk::IndexRecord::object_type);
        revision_offset = physical_offset(image, sample) + 0x14U;
    }
    for (const auto selector : {0U, 1U, 3U, 5U, 0xffffffffU}) {
        {
            std::array<std::byte, 4> bytes{};
            ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(0U, selector));
            std::fstream file{source, std::ios::in | std::ios::out | std::ios::binary};
            file.seekp(static_cast<std::streamoff>(revision_offset));
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ASSERT_TRUE(file);
        }
        const auto image = axk::open_image(source).value();
        const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id);
        ASSERT_TRUE(report) << report.error().message;
        for (const auto &profile : report->profiles)
            EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
    }
}

TEST_F(VolumeCapacityScan, InactiveWaveLaneUsesOnlyItsFirstByte) {
    std::uint64_t offset{};
    {
        const auto image = axk::open_image(source).value();
        const auto &sample = *std::ranges::find(image.partitions()[0].records, "SBNK", &axk::IndexRecord::object_type);
        offset = physical_offset(image, sample) + 0x88U;
    }
    {
        std::fstream file{source, std::ios::in | std::ios::out | std::ios::binary};
        file.seekp(static_cast<std::streamoff>(offset));
        const std::array<char, 2> inactive{0, 'Q'};
        file.write(inactive.data(), inactive.size());
        ASSERT_TRUE(file);
    }
    const auto image = axk::open_image(source).value();
    const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id);
    ASSERT_TRUE(report) << report.error().message;
    for (const auto &profile : report->profiles) {
        EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
        EXPECT_TRUE(profile.reasons.empty());
        EXPECT_EQ(profile.resident_bytes,
                  profile.baseline_bytes + 72U + (profile.target == axk::ASeriesLoadTarget::a3000 ? 300U : 336U));
    }
}

TEST_F(VolumeCapacityScan, OrdinaryVolumeFoldersAreNotEnumeratedAsSamplerCategories) {
    const auto reader = axk::FileReader::open(source).value();
    const std::array edits{axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"V", "NOTES"}}}};
    const auto prepared = axk::detail::prepare_sfs_file_edits(reader, {0U}, edits);
    ASSERT_TRUE(prepared);
    const auto image = axk::open_image(prepared->preview, source).value();
    const auto report = axk::inspect_volume_capacity(image, {0U}, volume_id);
    ASSERT_TRUE(report) << report.error().message;
    for (const auto &profile : report->profiles)
        EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
}

TEST_F(VolumeCapacityScan, LargeCategoryDirectoriesAreFullyInspected) {
    const auto image = axk::open_image(source).value();
    const auto &partition = image.partitions()[0];
    const auto &sample = *std::ranges::find(partition.records, "SBNK", &axk::IndexRecord::object_type);
    const auto &category = *std::ranges::find_if(partition.records, [](const auto &record) {
        return std::ranges::any_of(record.directory_entries, [](const auto &entry) { return entry.name == "Sample"; });
    });
    auto directory = image.read_record_data({0U}, category.sfs_id, 65536U).value();
    directory.resize(2051U * 32U);
    axk::ByteWriter rows{directory};
    for (unsigned index = 0U; index < 2049U; ++index) {
        const auto offset = static_cast<std::size_t>(index + 2U) * 32U;
        const auto name = index == 0U ? std::string{"Sample"} : std::format("Alias{:04}", index);
        ASSERT_TRUE(rows.write_be16(offset, 32U));
        ASSERT_TRUE(rows.write_be16(offset + 2U, static_cast<std::uint16_t>(name.size() + 1U)));
        ASSERT_TRUE(rows.write_be32(offset + 4U, sample.sfs_id.value));
        ASSERT_TRUE(rows.write_ascii_field(offset + 8U, 24U, name, std::byte{}));
    }
    const auto reader = axk::FileReader::open(source).value();
    std::vector<std::byte> bytes(static_cast<std::size_t>(reader->size()));
    ASSERT_TRUE(reader->read_exact_at(0U, bytes));
    const auto cluster_bytes = partition.sectors_per_cluster * image.superblock().sector_size_bytes;
    std::uint32_t first_cluster = partition.directory_index_cluster + partition.directory_index_span_clusters;
    for (const auto &record : partition.records)
        for (const auto &extent : record.extents)
            first_cluster = std::max(first_cluster, extent.cluster_offset + extent.cluster_count);
    const auto clusters = static_cast<std::uint32_t>((directory.size() + cluster_bytes - 1U) / cluster_bytes);
    ASSERT_LT(first_cluster + clusters, partition.cluster_count);
    const auto base = std::uint64_t{partition.start_sector} * image.superblock().sector_size_bytes;
    const auto data_offset = base + std::uint64_t{first_cluster} * cluster_bytes;
    std::ranges::copy(directory, bytes.begin() + static_cast<std::ptrdiff_t>(data_offset));
    axk::ByteWriter record{std::span{bytes}.subspan(static_cast<std::size_t>(category.record_offset.value), 0x48U)};
    ASSERT_TRUE(record.write_be16(0U, 1U));
    ASSERT_TRUE(record.write_be16(4U, static_cast<std::uint16_t>(clusters)));
    ASSERT_TRUE(record.write_be32(6U, static_cast<std::uint32_t>(directory.size())));
    ASSERT_TRUE(record.write_be32(0x0aU, first_cluster));
    ASSERT_TRUE(record.write_be32(0x0eU, clusters));
    ASSERT_TRUE(record.write_be32(0x12U, static_cast<std::uint32_t>(directory.size())));
    ASSERT_TRUE(axk::ByteWriter{bytes}.write_be16(static_cast<std::size_t>(sample.record_offset.value) + 0x46U, 2049U));
    const auto bitmap =
        axk::detail::sfs_allocation_bitmap_layout(partition.start_sector, partition.cluster_count,
                                                  partition.sectors_per_cluster, partition.bitmap_copy1_cluster,
                                                  partition.bitmap_copy2_cluster, image.superblock().sector_size_bytes)
            .value();
    for (const auto offset : {bitmap.bitmap_copy1_offset, bitmap.bitmap_copy2_offset}) {
        const auto mark = [&](std::uint32_t cluster, bool used) {
            auto &byte = bytes[static_cast<std::size_t>(offset) + cluster / 8U];
            const auto mask = static_cast<std::byte>(0x80U >> (cluster & 7U));
            byte = used ? byte | mask : byte & ~mask;
        };
        for (const auto &extent : category.extents)
            for (std::uint32_t index = 0U; index < extent.cluster_count; ++index)
                mark(extent.cluster_offset + index, false);
        for (std::uint32_t index = 0U; index < clusters; ++index)
            mark(first_cluster + index, true);
    }
    auto metadata = std::make_shared<MetadataReader>(std::make_shared<axk::MemoryReader>(std::move(bytes)));
    const auto large = axk::open_image(metadata, source).value();
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(large.partitions()[0].allocation));
    metadata->armed = true;
    const auto report = axk::inspect_volume_capacity(large, {0U}, volume_id);
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_LE(metadata->largest_read, 65536U);
    for (const auto &profile : report->profiles) {
        EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
        EXPECT_EQ(profile.minimum_resident_slots, profile.baseline_slots + 2U);
    }
}

TEST_F(VolumeCapacityScan, FilesPutsAndMovesCannotSkipSamplerVolumeAdmission) {
    const auto reader = axk::FileReader::open(source).value();
    const axk::VolumeCapacityPolicy policy{axk::ASeriesLoadTarget::a3000};
    for (const axk::FilesystemEdit &edit :
         {axk::FilesystemEdit{axk::PutFilesystemFile{{"V", "SBNK", "Sample"}, reader, axk::FileConflict::replace}},
          axk::FilesystemEdit{axk::MoveFilesystemEntry{{"V", "SBNK", "Sample"}, {"V", "SBNK"}}}}) {
        const std::array edits{edit};
        const auto review = axk::detail::inspect_filesystem_capacity(reader, reader, {0U}, edits, policy);
        ASSERT_TRUE(review) << review.error().message;
        ASSERT_EQ(review->reports.size(), 1U);
        EXPECT_TRUE(review->allowed);
        EXPECT_EQ(review->reports.front().profiles[0].status, axk::VolumeCapacityStatus::fits);
    }
    const std::array edits{axk::FilesystemEdit{axk::PutFilesystemFile{{"Missing", "SEQU", "X"}, reader}}};
    EXPECT_FALSE(axk::detail::inspect_filesystem_capacity(reader, reader, {0U}, edits, policy));
}

TEST_F(VolumeCapacityScan, SkippingAnExistingSamplerFileDoesNotRequireCapacityApproval) {
    const std::array edits{
        axk::FilesystemEdit{axk::PutFilesystemFile{
            {"V", "SBNK", "Sample"}, std::make_shared<axk::MemoryReader>(std::vector<std::byte>{std::byte{1}})}},
        axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"Archive"}}}};
    const auto review = axk::inspect_sfs_file_edit_capacity(source, {0U}, edits, {axk::ASeriesLoadTarget::a3000});
    ASSERT_TRUE(review) << review.error().message;
    EXPECT_TRUE(review->allowed);
    EXPECT_TRUE(review->reports.empty());
}

TEST_F(VolumeCapacityScan, CategoryActivationAndRootRenameAreInspectedTogether) {
    const auto reader = axk::FileReader::open(source).value();
    const std::vector<axk::FilesystemEdit> edits{axk::RenameFilesystemEntry{{"V", "SBNK"}, "ARCHIVE"},
                                                 axk::RenameFilesystemEntry{{"V", "ARCHIVE"}, "SBNK"},
                                                 axk::RenameFilesystemEntry{{"V"}, "V2"}};
    const auto prepared = axk::detail::prepare_sfs_file_edits(reader, {0U}, edits);
    ASSERT_TRUE(prepared) << prepared.error().message;
    const auto review = axk::detail::inspect_filesystem_capacity(reader, prepared->preview, {0U}, edits,
                                                                 {axk::ASeriesLoadTarget::a3000});
    ASSERT_TRUE(review) << review.error().message;
    ASSERT_EQ(review->reports.size(), 1U);
    EXPECT_EQ(review->reports.front().volume_name, "V2");
    EXPECT_TRUE(review->allowed);
    EXPECT_TRUE(axk::enforce_volume_capacity_admission(*review, {review->target}));
}

TEST_F(VolumeCapacityScan, OrdinaryRootRenameDoesNotRequireAdmission) {
    const auto reader = axk::FileReader::open(source).value();
    const std::array edits{axk::FilesystemEdit{axk::RenameFilesystemEntry{{"V"}, "V2"}}};
    const auto prepared = axk::detail::prepare_sfs_file_edits(reader, {0U}, edits);
    ASSERT_TRUE(prepared);
    const auto review = axk::detail::inspect_filesystem_capacity(reader, prepared->preview, {0U}, edits,
                                                                 {axk::ASeriesLoadTarget::a3000});
    ASSERT_TRUE(review);
    EXPECT_TRUE(review->allowed);
    EXPECT_TRUE(review->reports.empty());
}

TEST_F(VolumeCapacityScan, MovingAnEntireWaveCategoryCannotLeaveAnUncheckedSource) {
    const auto reader = axk::FileReader::open(source).value();
    const std::array create{axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"Archive"}}}};
    const auto created = axk::detail::prepare_sfs_file_edits(reader, {0U}, create);
    ASSERT_TRUE(created);
    const std::array edits{axk::FilesystemEdit{axk::MoveFilesystemEntry{{"V", "SMPL"}, {"Archive"}}}};
    const auto prepared = axk::detail::prepare_sfs_file_edits(created->preview, {0U}, edits);
    ASSERT_TRUE(prepared) << prepared.error().message;
    const auto review = axk::detail::inspect_filesystem_capacity(created->preview, prepared->preview, {0U}, edits,
                                                                 {axk::ASeriesLoadTarget::a4000_a5000});
    ASSERT_TRUE(review) << review.error().message;
    EXPECT_TRUE(review->allowed);
    EXPECT_TRUE(std::ranges::any_of(review->reports, [](const auto &report) {
        return report.volume_name == "V" && report.profiles[1].status == axk::VolumeCapacityStatus::fits &&
               std::ranges::any_of(report.profiles[1].reasons,
                                   [](const auto &reason) { return reason.code == "SAMPLE_SKIPPED_MISSING_WAVE"; });
    }));
}

TEST_F(VolumeCapacityScan, MovingAWaveToAnOrdinaryFolderStillChecksTheSource) {
    const auto reader = axk::FileReader::open(source).value();
    const std::array create{axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"V", "NOTES"}}}};
    const auto created = axk::detail::prepare_sfs_file_edits(reader, {0U}, create);
    ASSERT_TRUE(created);
    const std::array edits{axk::FilesystemEdit{axk::MoveFilesystemEntry{{"V", "SMPL", "Wave"}, {"V", "NOTES"}}}};
    const auto prepared = axk::detail::prepare_sfs_file_edits(created->preview, {0U}, edits);
    ASSERT_TRUE(prepared);
    const auto review = axk::detail::inspect_filesystem_capacity(created->preview, prepared->preview, {0U}, edits, {});
    ASSERT_TRUE(review) << review.error().message;
    EXPECT_TRUE(review->allowed);
    ASSERT_EQ(review->reports.size(), 1U);
    EXPECT_TRUE(std::ranges::any_of(review->reports.front().profiles[1].reasons,
                                    [](const auto &reason) { return reason.code == "SAMPLE_SKIPPED_MISSING_WAVE"; }));
    ASSERT_EQ(review->reports.size(), 1U);
    EXPECT_EQ(review->reports.front().volume_name, "V");
}

TEST_F(VolumeCapacityScan, PlainFilesAndMovingAVolumeOutOfLoadScopeStayEditable) {
    const auto reader = axk::FileReader::open(source).value();
    const std::vector<axk::FilesystemEdit> plain{
        axk::CreateFilesystemDirectory{{"V", "NOTES"}},
        axk::PutFilesystemFile{{"V", "NOTES", "readme"},
                               std::make_shared<axk::MemoryReader>(std::vector<std::byte>{std::byte{1}})}};
    const auto plain_review = axk::inspect_sfs_file_edit_capacity(source, {0U}, plain, {axk::ASeriesLoadTarget::a3000});
    ASSERT_TRUE(plain_review) << plain_review.error().message;
    EXPECT_TRUE(plain_review->allowed);
    EXPECT_TRUE(plain_review->reports.empty());
    const std::array create{axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"Archive"}}}};
    const auto created = axk::detail::prepare_sfs_file_edits(reader, {0U}, create);
    ASSERT_TRUE(created);
    const std::array moved{axk::FilesystemEdit{axk::MoveFilesystemEntry{{"V"}, {"Archive"}}}};
    const auto prepared = axk::detail::prepare_sfs_file_edits(created->preview, {0U}, moved);
    ASSERT_TRUE(prepared);
    const auto move_review = axk::detail::inspect_filesystem_capacity(created->preview, prepared->preview, {0U}, moved,
                                                                      {axk::ASeriesLoadTarget::a3000});
    ASSERT_TRUE(move_review) << move_review.error().message;
    EXPECT_TRUE(move_review->allowed);
    EXPECT_TRUE(move_review->reports.empty());
}

TEST_F(VolumeCapacityScan, PublicWriterLoadsLaterSamplesOnA3000WithoutApproval) {
    axk::VolumeSpec volume;
    volume.name = "Review";
    volume.waveforms.push_back({"wave", "Wave", folder / "wave.wav", 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Sample";
    sample.waveform_id = "wave";
    volume.samples.push_back(sample);
    const axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {{"P", {volume}}}};
    const axk::VolumeCapacityPolicy policy{axk::ASeriesLoadTarget::a3000};
    const auto review = axk::inspect_hds_build_capacity(manifest, policy);
    ASSERT_TRUE(review) << review.error().message;
    EXPECT_TRUE(review->allowed);
    ASSERT_TRUE(axk::write_hds_image(manifest, folder / "initial.hds", false, {}, policy));
    axk::Waveform changed;
    changed.format = {1U, 2U, 44100U};
    changed.frame_count = 4096U;
    changed.pcm.resize(8192U, std::byte{1});
    ASSERT_TRUE(axk::write_wav_atomic(folder / "wave.wav", changed, true));
    ASSERT_TRUE(axk::write_hds_image(manifest, folder / "changed.hds", false, {}, policy));
    const auto image = axk::open_image(folder / "changed.hds").value();
    const auto &wave = *std::ranges::find(image.partitions()[0].records, "SMPL", &axk::IndexRecord::object_type);
    const auto metadata = axk::decode_object(image.read_record_range({0U}, wave.sfs_id, 0U, 0xacU).value()).value();
    const auto &payload = std::get<axk::CurrentSmpl>(metadata.payload);
    EXPECT_EQ(image.read_record_range({0U}, wave.sfs_id, payload.stored_pcm_offset, 8192U).value(), changed.pcm);
}

TEST_F(VolumeCapacityScan, SmallerSequenceReplacementCanReduceAnAlreadyOversizedVolume) {
    const std::array midi{
        std::byte{'M'}, std::byte{'T'}, std::byte{'h'},  std::byte{'d'},  std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{6},   std::byte{0},   std::byte{0},    std::byte{0},    std::byte{1}, std::byte{0}, std::byte{96},
        std::byte{'M'}, std::byte{'T'}, std::byte{'r'},  std::byte{'k'},  std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{4},   std::byte{0},   std::byte{0xff}, std::byte{0x2f}, std::byte{0}};
    auto sequence = axk::smf0_to_current_sequence(midi, "Sequence", axk::SequenceSystemExclusivePolicy::reject);
    ASSERT_TRUE(sequence);
    sequence->resize(800048U);
    ASSERT_TRUE(axk::ByteWriter{*sequence}.write_be32(0x18U, 800000U));
    const auto reader = axk::FileReader::open(source).value();
    const std::array initial{axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"W               "}}},
                             axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"W", "SEQU"}}},
                             axk::FilesystemEdit{axk::PutFilesystemFile{
                                 {"W", "SEQU", "Sequence        "}, std::make_shared<axk::MemoryReader>(*sequence)}},
                             axk::FilesystemEdit{axk::PutFilesystemFile{
                                 {"V", "SEQU", "Sequence        "}, std::make_shared<axk::MemoryReader>(*sequence)}}};
    // Deliberately construct an oversized starting fixture without production admission.
    const auto prepared = axk::detail::prepare_sfs_file_edits(reader, {0U}, initial);
    ASSERT_TRUE(prepared) << prepared.error().message;
    const auto image = axk::open_image(prepared->preview, {}).value();
    ASSERT_EQ(axk::inspect_volume_capacity(image, {0U}, volume_id)->profiles[1].status,
              axk::VolumeCapacityStatus::does_not_fit);
    sequence->resize(790048U);
    ASSERT_TRUE(axk::ByteWriter{*sequence}.write_be32(0x18U, 790000U));
    const std::array smaller{
        axk::FilesystemEdit{axk::PutFilesystemFile{
            {"V", "SEQU", "Sequence"}, std::make_shared<axk::MemoryReader>(*sequence), axk::FileConflict::replace}},
        axk::FilesystemEdit{axk::PutFilesystemFile{
            {"W", "SEQU", "Sequence"}, std::make_shared<axk::MemoryReader>(*sequence), axk::FileConflict::replace}},
        axk::FilesystemEdit{axk::CreateFilesystemDirectory{{"Archive"}}}};
    const auto changed = axk::detail::prepare_sfs_file_edits(prepared->preview, {0U}, smaller);
    ASSERT_TRUE(changed) << changed.error().message;
    const auto review =
        axk::detail::inspect_filesystem_capacity(prepared->preview, changed->preview, {0U}, smaller, {});
    ASSERT_TRUE(review) << review.error().message;
    EXPECT_TRUE(review->allowed);
    EXPECT_TRUE(review->reports.empty());
    sequence->resize(810048U);
    ASSERT_TRUE(axk::ByteWriter{*sequence}.write_be32(0x18U, 810000U));
    const std::array larger{axk::FilesystemEdit{axk::PutFilesystemFile{
        {"V", "SEQU", "Sequence"}, std::make_shared<axk::MemoryReader>(*sequence), axk::FileConflict::replace}}};
    const auto grown = axk::detail::prepare_sfs_file_edits(prepared->preview, {0U}, larger);
    ASSERT_TRUE(grown) << grown.error().message;
    const auto refused = axk::detail::inspect_filesystem_capacity(prepared->preview, grown->preview, {0U}, larger, {});
    ASSERT_TRUE(refused);
    EXPECT_FALSE(refused->allowed);
}

TEST_F(VolumeCapacityScan, GrowthThenVolumeRenameUsesFinalPhysicalDestination) {
    axk::InsertWaveformOperation insert;
    insert.partition = axk::PartitionIndex{0U};
    insert.volume_name = "V";
    insert.waveform = {folder / "wave.wav", {"Second"}, 60U, {}};
    axk::RenameVolumeOperation rename{axk::PartitionIndex{0U}, "V", "V2"};
    axk::AlterationManifest manifest;
    manifest.schema_version = "1.0";
    manifest.operations = {{"insert", insert}, {"rename", rename}};
    const auto inspection = axk::inspect_hds_alteration(source, manifest);
    ASSERT_TRUE(inspection) << inspection.error().message;
    ASSERT_EQ(inspection->capacity.reports.size(), 1U);
    EXPECT_EQ(inspection->capacity.reports.front().volume_name, "V2");
    EXPECT_TRUE(inspection->capacity.allowed);
    ASSERT_TRUE(axk::alter_hds(source, manifest, folder / "renamed.hds"));
}

TEST_F(VolumeCapacityScan, NewProgramReferencesAreInspectedWithoutPayloadGrowth) {
    axk::VolumeSpec volume;
    volume.name = "Programs";
    volume.waveforms.push_back({"wave", "Wave", folder / "wave.wav", 60U, {}});
    for (const auto name : {"Standalone", "Member"}) {
        axk::SampleSpec sample;
        sample.name = name;
        sample.waveform_id = "wave";
        volume.samples.push_back(sample);
    }
    volume.sample_banks.push_back({"Bank", {"Member"}});
    volume.programs.push_back({1U, "Program", {{"SBNK", "Standalone"}}});
    const auto image_path = folder / "programs.hds";
    ASSERT_TRUE(axk::write_hds_image({"1.0", 8U * 1024U * 1024U, {{"P", {volume}}}}, image_path));
    const auto image = axk::open_image(image_path).value();
    const auto &program = *std::ranges::find(image.partitions()[0].records, "PROG", &axk::IndexRecord::object_type);
    const auto payload = image.read_record_range({0U}, program.sfs_id, 0U, program.data_size).value();
    axk::ReplaceProgramAssignmentsOperation replacement;
    replacement.partition = axk::PartitionIndex{0U};
    replacement.volume_name = "Programs";
    replacement.program_number = 1U;
    replacement.expected_payload_sha256 = axk::package_internal::hex_digest(axk::package_internal::sha256(payload));
    replacement.assignments.push_back({{}, axk::ProgramAssignmentSpec{"SBNK", "Member"}});
    const axk::AlterationManifest manifest{"1.0", {{"assign", replacement}}};
    const auto inspection = axk::inspect_hds_alteration(image_path, manifest);
    ASSERT_TRUE(inspection) << inspection.error().message;
    ASSERT_EQ(inspection->capacity.reports.size(), 1U);
    EXPECT_EQ(inspection->capacity.reports.front().volume_name, "Programs");
    EXPECT_TRUE(inspection->capacity.allowed);
    const auto &profile = inspection->capacity.reports.front().profiles[1];
    EXPECT_EQ(profile.status, axk::VolumeCapacityStatus::fits);
    EXPECT_EQ(profile.resident_bytes, 111280U + 336U * 3U + 472U + 72U);
    ASSERT_TRUE(axk::alter_hds(image_path, manifest, folder / "assigned.hds"));
}

} // namespace
