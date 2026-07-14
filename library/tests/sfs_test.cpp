#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/io.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"

namespace {

constexpr std::size_t sector_size = 512;

void write_magic(std::span<std::byte> bytes, std::size_t offset) {
    const std::string value{"YAMAHA_dev3"};
    std::transform(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                   [](char character) { return static_cast<std::byte>(character); });
}

void write_ascii(std::span<std::byte> bytes, std::size_t offset, std::string_view value) {
    std::transform(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                   [](char character) { return static_cast<std::byte>(character); });
}

void write_directory_entry(axk::ByteWriter &writer, std::span<std::byte> bytes, std::size_t offset,
                           std::string_view name, std::uint32_t link) {
    ASSERT_TRUE(writer.write_be16(offset, 0x20));
    ASSERT_TRUE(writer.write_be16(offset + 2, static_cast<std::uint16_t>(name.size() + 1U)));
    ASSERT_TRUE(writer.write_be32(offset + 4, link));
    write_ascii(bytes, offset + 8, name);
}

std::vector<std::byte> image_fixture() {
    constexpr std::size_t sectors = 2048;
    std::vector<std::byte> bytes(sectors * sector_size);
    axk::ByteWriter writer{bytes};
    for (const std::size_t sector : {0U, 1U}) {
        const auto base = sector * sector_size;
        write_magic(bytes, base);
        EXPECT_TRUE(writer.write_be32(base + 0x9c, sector_size));
        EXPECT_TRUE(writer.write_be32(base + 0xa0, sectors));
        EXPECT_TRUE(writer.write_be32(base + 0xa8, 3));
        EXPECT_TRUE(writer.write_be32(base + 0xac, sectors - 3));
    }

    const auto partition = 3U * sector_size;
    write_magic(bytes, partition);
    write_ascii(bytes, partition + 0x40, "Test Partition");
    EXPECT_TRUE(writer.write_be32(partition + 0x90, 1022));
    EXPECT_TRUE(writer.write_be32(partition + 0x94, 2));
    EXPECT_TRUE(writer.write_be32(partition + 0x9c, 3));
    EXPECT_TRUE(writer.write_be32(partition + 0xa4, 4));
    EXPECT_TRUE(writer.write_be32(partition + 0xa8, 2));
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(partition), 1024,
                bytes.begin() + static_cast<std::ptrdiff_t>(partition + 1024));

    const auto index = 11U * sector_size;
    EXPECT_TRUE(writer.write_be16(index, 1));
    EXPECT_TRUE(writer.write_be16(index + 4, 1));
    EXPECT_TRUE(writer.write_be32(index + 6, 96));
    EXPECT_TRUE(writer.write_be32(index + 0x0a, 6));
    EXPECT_TRUE(writer.write_be32(index + 0x0e, 1));
    EXPECT_TRUE(writer.write_be32(index + 0x12, 96));

    const auto directory = 15U * sector_size;
    write_directory_entry(writer, bytes, directory, ".", 0);
    write_directory_entry(writer, bytes, directory + 32, "..", 0);
    write_directory_entry(writer, bytes, directory + 64, "Samples", 4);
    const auto bitmap = 9U * sector_size;
    bytes[bitmap + 6U / 8U] |= static_cast<std::byte>(0x80U >> (6U & 7U));
    return bytes;
}

std::vector<std::byte> large_object_fixture() {
    auto bytes = image_fixture();
    axk::ByteWriter writer{bytes};
    const auto index = 11U * sector_size;
    EXPECT_TRUE(writer.write_be16(index + 4, 200));
    EXPECT_TRUE(writer.write_be32(index + 6, 200000));
    EXPECT_TRUE(writer.write_be32(index + 0x0e, 200));
    EXPECT_TRUE(writer.write_be32(index + 0x12, 200000));
    const auto payload = 15U * sector_size;
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(payload), 512, std::byte{});
    write_ascii(bytes, payload, "FSFSDEV3SPLXSMPL");
    write_ascii(bytes, payload + 0x32, "Large waveform");
    const auto bitmap = 9U * sector_size;
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(bitmap), 128, std::byte{});
    for (std::uint32_t cluster = 6; cluster < 206; ++cluster) {
        bytes[bitmap + cluster / 8U] |= static_cast<std::byte>(0x80U >> (cluster & 7U));
    }
    return bytes;
}

std::vector<std::byte> continuation_fixture(bool cycle) {
    auto bytes = image_fixture();
    axk::ByteWriter writer{bytes};
    const auto index = 11U * sector_size;
    constexpr std::uint16_t extent_count = 48;
    EXPECT_TRUE(writer.write_be16(index, extent_count));
    EXPECT_TRUE(writer.write_be16(index + 4, extent_count));
    EXPECT_TRUE(writer.write_be32(index + 6, 96));
    EXPECT_TRUE(writer.write_be32(index + 0x0a, 6));

    const auto old_payload = 15U * sector_size;
    const std::vector<std::byte> logical(bytes.begin() + static_cast<std::ptrdiff_t>(old_payload),
                                         bytes.begin() + static_cast<std::ptrdiff_t>(old_payload + 96U));
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(old_payload), 1024, std::byte{});
    EXPECT_TRUE(writer.write_be32(old_payload, cycle ? 1U : extent_count));
    EXPECT_TRUE(writer.write_be32(old_payload + 8, cycle ? 6U : 0U));
    const auto written_extents = cycle ? 1U : extent_count;
    for (std::uint32_t extent = 0; extent < written_extents; ++extent) {
        const auto item = old_payload + 12U + extent * 12U;
        EXPECT_TRUE(writer.write_be32(item, 7U + extent));
        EXPECT_TRUE(writer.write_be32(item + 4, 1));
        EXPECT_TRUE(writer.write_be32(item + 8, 2));
        const auto target = (3U + (7U + extent) * 2U) * sector_size;
        std::copy_n(logical.begin() + static_cast<std::ptrdiff_t>(extent * 2U), 2,
                    bytes.begin() + static_cast<std::ptrdiff_t>(target));
    }
    const auto bitmap = 9U * sector_size;
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(bitmap), 128, std::byte{});
    const auto last_cluster = cycle ? 7U : 54U;
    for (std::uint32_t cluster = 6; cluster <= last_cluster; ++cluster) {
        bytes[bitmap + cluster / 8U] |= static_cast<std::byte>(0x80U >> (cluster & 7U));
    }
    return bytes;
}

std::vector<std::byte> alternating_object_fixture() {
    auto bytes = large_object_fixture();
    constexpr std::array prefix{
        std::byte{'F'}, std::byte{0x55}, std::byte{'F'}, std::byte{0xaa}, std::byte{'D'}, std::byte{0x55},
        std::byte{'V'}, std::byte{0xaa}, std::byte{'S'}, std::byte{0x55}, std::byte{'L'}, std::byte{0xaa},
        std::byte{'S'}, std::byte{0x55}, std::byte{'P'}, std::byte{0xaa},
    };
    std::copy(prefix.begin(), prefix.end(), bytes.begin() + 15U * sector_size);
    return bytes;
}

class TrackingReader final : public axk::RandomAccessReader {
  public:
    explicit TrackingReader(std::vector<std::byte> bytes) : memory_{std::move(bytes)} {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return memory_.size(); }

    [[nodiscard]] axk::Result<void> read_exact_at(std::uint64_t offset,
                                                  std::span<std::byte> destination) const override {
        reads.emplace_back(offset, destination.size());
        return memory_.read_exact_at(offset, destination);
    }

    mutable std::vector<std::pair<std::uint64_t, std::size_t>> reads;

  private:
    axk::MemoryReader memory_;
};

class SparseReader final : public axk::RandomAccessReader {
  public:
    explicit SparseReader(std::uint64_t size) : size_{size} {}

    void add(std::uint64_t offset, std::vector<std::byte> bytes) { patches_.emplace_back(offset, std::move(bytes)); }

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] axk::Result<void> read_exact_at(std::uint64_t offset,
                                                  std::span<std::byte> destination) const override {
        const auto end = axk::checked_add(offset, destination.size());
        if (!end || *end > size_) {
            return std::unexpected{
                axk::make_error(axk::ErrorCode::io_short_read, axk::ErrorCategory::io, "sparse fixture short read")};
        }
        std::fill(destination.begin(), destination.end(), std::byte{});
        for (const auto &[patch_offset, patch] : patches_) {
            const auto patch_end = patch_offset + patch.size();
            const auto overlap_start = std::max(offset, patch_offset);
            const auto overlap_end = std::min(*end, patch_end);
            if (overlap_start >= overlap_end) {
                continue;
            }
            std::copy(patch.begin() + static_cast<std::ptrdiff_t>(overlap_start - patch_offset),
                      patch.begin() + static_cast<std::ptrdiff_t>(overlap_end - patch_offset),
                      destination.begin() + static_cast<std::ptrdiff_t>(overlap_start - offset));
        }
        return {};
    }

  private:
    std::uint64_t size_{};
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> patches_;
};

std::shared_ptr<SparseReader> sparse_geometry_fixture(std::uint64_t total_sectors, std::uint8_t partition_count) {
    auto sparse = std::make_shared<SparseReader>(total_sectors * sector_size);
    std::vector<std::byte> superblock(sector_size);
    axk::ByteWriter superblock_writer{superblock};
    write_magic(superblock, 0);
    EXPECT_TRUE(superblock_writer.write_be32(0x9c, sector_size));
    EXPECT_TRUE(superblock_writer.write_be32(0xa0, static_cast<std::uint32_t>(total_sectors)));
    const auto slot_span = std::min<std::uint64_t>((total_sectors - 2U) / partition_count, 2'097'151U);
    for (std::uint8_t index = 0; index < partition_count; ++index) {
        const auto start = 3U + static_cast<std::uint64_t>(index) * slot_span;
        const auto count = slot_span - 1U;
        EXPECT_TRUE(superblock_writer.write_be32(0xa8U + index * 8U, static_cast<std::uint32_t>(start)));
        EXPECT_TRUE(superblock_writer.write_be32(0xacU + index * 8U, static_cast<std::uint32_t>(count)));
        std::vector<std::byte> header(1024);
        axk::ByteWriter header_writer{header};
        write_magic(header, 0);
        write_ascii(header, 0x40, "Partition " + std::to_string(index));
        EXPECT_TRUE(header_writer.write_be32(0x90, static_cast<std::uint32_t>(count / 2U)));
        EXPECT_TRUE(header_writer.write_be32(0x94, 2));
        EXPECT_TRUE(header_writer.write_be32(0x9c, 3));
        EXPECT_TRUE(header_writer.write_be32(0xa4, 4));
        EXPECT_TRUE(header_writer.write_be32(0xa8, 358));
        const auto header_offset = start * sector_size;
        sparse->add(header_offset, header);
        sparse->add(header_offset + 1024U, std::move(header));
    }
    sparse->add(0, superblock);
    sparse->add(sector_size, std::move(superblock));
    return sparse;
}

TEST(SfsReader, MatchesMaintainedSemanticContractsOnFixtures) {
    struct Expected {
        std::string_view filename;
        std::uint32_t allocated;
        std::uint64_t free_kib;
        std::size_t known_record_count;
        std::string_view last_known_type;
        std::string_view last_known_name;
    };
    constexpr std::array cases{
        Expected{"HD00_512_single_sbnk_authored.hds", 447, 213, 25, "SBNK", "_NewSample"},
        Expected{"HD00_512_multi_sbnk_authored.hds", 477, 183, 40, "SBNK", "JS03   *********"},
    };
    const auto root = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored";

    for (const auto &expected : cases) {
        const auto result = axk::open_image(root / expected.filename);
        ASSERT_TRUE(result) << expected.filename;
        ASSERT_EQ(result->partitions().size(), 1U) << expected.filename;
        const auto &partition = result->partitions()[0];
        EXPECT_EQ(partition.cluster_count, 1022U);
        EXPECT_EQ(partition.directory_index_span_clusters, 358U);
        EXPECT_EQ(partition.allocation.stored_used_cluster_count, expected.allocated);
        EXPECT_EQ(partition.allocation.reconstructed_used_cluster_count, expected.allocated);
        ASSERT_TRUE(partition.allocation.free_space);
        EXPECT_EQ(partition.allocation.free_space->sampler_visible_free_kib, expected.free_kib);
        ASSERT_GE(partition.records.size(), expected.known_record_count);
        const auto &last = partition.records[expected.known_record_count - 1U];
        EXPECT_EQ(last.object_type, expected.last_known_type);
        EXPECT_EQ(last.object_name, expected.last_known_name);
        EXPECT_TRUE(partition.allocation.stored_not_reconstructed.empty());
        EXPECT_TRUE(partition.allocation.reconstructed_not_stored.empty());
    }
}

} // namespace

TEST(SfsFreeSpace, MatchesHardwareFormulaAndRejectsImpossibleGeometry) {
    const auto result = axk::calculate_sfs_free_space(1000, 100, 305, 1024);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->reserved_cluster_count, 100U);
    EXPECT_EQ(result->free_cluster_count, 595U);
    EXPECT_EQ(result->sampler_visible_free_kib, 595U);
    EXPECT_FALSE(axk::calculate_sfs_free_space(100, 101, 0));
    EXPECT_FALSE(axk::calculate_sfs_free_space(100, 50, 51));
}

TEST(SfsReader, ParsesDuplicatedGeometryDirectoriesAllocationAndFreeSpace) {
    auto reader = std::make_shared<axk::MemoryReader>(image_fixture());
    const auto result = axk::open_image(reader, "fixture.hds");
    ASSERT_TRUE(result) << axk::render_error(result.error());
    EXPECT_EQ(result->image_size_bytes(), 2048U * sector_size);
    EXPECT_TRUE(result->backup_superblock_matches());
    ASSERT_EQ(result->partitions().size(), 1U);
    const auto &partition = result->partitions()[0];
    EXPECT_EQ(partition.name, "Test Partition");
    EXPECT_TRUE(partition.backup_header_matches);
    ASSERT_EQ(partition.records.size(), 1U);
    EXPECT_EQ(partition.records[0].payload_kind, axk::PayloadKind::directory);
    ASSERT_EQ(partition.records[0].directory_entries.size(), 3U);
    EXPECT_EQ(partition.records[0].directory_entries[2].name, "Samples");
    EXPECT_EQ(partition.allocation.stored_used_cluster_count, 1U);
    EXPECT_EQ(partition.allocation.reconstructed_used_cluster_count, 1U);
    ASSERT_TRUE(partition.allocation.free_space);
    EXPECT_EQ(partition.allocation.free_space->reserved_cluster_count, 6U);
    EXPECT_EQ(partition.allocation.free_space->free_cluster_count, 1015U);
}

TEST(SfsReader, KeepsBackupDisagreementAsDiagnostic) {
    auto bytes = image_fixture();
    bytes[sector_size + 0x80] = std::byte{0x55};
    auto reader = std::make_shared<axk::MemoryReader>(std::move(bytes));
    const auto result = axk::open_image(reader, "fixture.hds");
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->backup_superblock_matches());
    ASSERT_FALSE(result->diagnostics().empty());
    EXPECT_EQ(result->diagnostics()[0].code, axk::ErrorCode::container_backup_mismatch);
}

TEST(SfsReader, InventoryReadsOnlyAnObjectPrefixNotWaveformPayload) {
    auto reader = std::make_shared<TrackingReader>(large_object_fixture());
    const auto result = axk::open_image(reader, "large-object.hds");
    ASSERT_TRUE(result);
    ASSERT_EQ(result->partitions().size(), 1U);
    ASSERT_EQ(result->partitions()[0].records.size(), 1U);
    EXPECT_EQ(result->partitions()[0].records[0].object_name, "Large waveform");

    constexpr std::uint64_t payload_offset = 15U * sector_size;
    std::vector<std::size_t> payload_read_sizes;
    for (const auto &[offset, count] : reader->reads) {
        if (offset >= payload_offset) {
            payload_read_sizes.push_back(count);
        }
    }
    ASSERT_EQ(payload_read_sizes.size(), 1U);
    EXPECT_EQ(payload_read_sizes[0], 512U);
}

TEST(SfsReader, ClassifiesEstablishedAlternatingByteObjectType) {
    auto reader = std::make_shared<axk::MemoryReader>(alternating_object_fixture());
    const auto result = axk::open_image(reader, "alternating-object.hds");
    ASSERT_TRUE(result);
    ASSERT_EQ(result->partitions().size(), 1U);
    ASSERT_EQ(result->partitions()[0].records.size(), 1U);
    const auto &record = result->partitions()[0].records[0];
    EXPECT_EQ(record.payload_kind, axk::PayloadKind::alternating_byte_object);
    EXPECT_EQ(record.object_type, "SMPL");
    EXPECT_TRUE(record.object_name.empty());
}

TEST(SfsReader, EnumeratesEverySupportedPartitionCountAtMinimumAndTwoGiB) {
    constexpr std::uint64_t two_gib_sectors = (std::uint64_t{2} * 1024U * 1024U * 1024U) / sector_size;
    for (std::uint8_t count = 1; count <= 8; ++count) {
        for (const auto total_sectors : {
                 2U + static_cast<std::uint64_t>(count) * 2046U,
                 two_gib_sectors,
             }) {
            const auto reader = sparse_geometry_fixture(total_sectors, count);
            const auto result = axk::open_image(reader, "sparse-geometry.hds");
            ASSERT_TRUE(result) << "partitions=" << static_cast<unsigned int>(count) << " sectors=" << total_sectors;
            ASSERT_EQ(result->partitions().size(), count);
            for (std::uint8_t index = 0; index < count; ++index) {
                EXPECT_EQ(result->partitions()[index].index.value, index);
                EXPECT_EQ(result->partitions()[index].name, "Partition " + std::to_string(index));
            }
        }
    }
}

TEST(SfsReader, ResolvesFragmentedFortyEightExtentDirectoryAndListAllocation) {
    auto reader = std::make_shared<axk::MemoryReader>(continuation_fixture(false));
    const auto result = axk::open_image(reader, "fragmented.hds");
    ASSERT_TRUE(result);
    ASSERT_EQ(result->partitions().size(), 1U);
    const auto &partition = result->partitions()[0];
    ASSERT_EQ(partition.records.size(), 1U);
    const auto &record = partition.records[0];
    EXPECT_EQ(record.extents.size(), 48U);
    EXPECT_EQ(record.continuation_clusters, std::vector<std::uint32_t>{6});
    EXPECT_EQ(record.payload_kind, axk::PayloadKind::directory);
    ASSERT_EQ(record.directory_entries.size(), 3U);
    EXPECT_EQ(record.directory_entries[2].name, "Samples");
    EXPECT_EQ(partition.allocation.stored_used_cluster_count, 49U);
    EXPECT_EQ(partition.allocation.reconstructed_used_cluster_count, 49U);
    EXPECT_TRUE(partition.allocation.stored_not_reconstructed.empty());
    EXPECT_TRUE(partition.allocation.reconstructed_not_stored.empty());
}

TEST(SfsReader, ReportsContinuationCycleAndBitmapMismatchWithoutRepair) {
    auto cycle_reader = std::make_shared<axk::MemoryReader>(continuation_fixture(true));
    const auto cycle = axk::open_image(cycle_reader, "cycle.hds");
    ASSERT_TRUE(cycle);
    ASSERT_EQ(cycle->partitions().size(), 1U);
    const auto &cycle_partition = cycle->partitions()[0];
    EXPECT_EQ(cycle_partition.allocation.invalid_extent_record_count, 1U);
    EXPECT_TRUE(std::any_of(cycle_partition.diagnostics.begin(), cycle_partition.diagnostics.end(),
                            [](const axk::Error &error) { return error.code == axk::ErrorCode::allocation_cycle; }));

    auto mismatch_bytes = continuation_fixture(false);
    const auto bitmap = 9U * sector_size;
    mismatch_bytes[bitmap + 54U / 8U] &= static_cast<std::byte>(0xffU ^ (0x80U >> (54U & 7U)));
    auto mismatch_reader = std::make_shared<axk::MemoryReader>(std::move(mismatch_bytes));
    const auto mismatch = axk::open_image(mismatch_reader, "mismatch.hds");
    ASSERT_TRUE(mismatch);
    const auto &ranges = mismatch->partitions()[0].allocation.reconstructed_not_stored;
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_EQ(ranges[0].start_cluster, 54U);
    EXPECT_EQ(ranges[0].end_cluster, 54U);
    const auto validation = axk::validate_semantics(*mismatch, {}, {});
    const auto issue = std::ranges::find(validation.issues, "SFS_ALLOCATION_MISMATCH", &axk::ValidationIssue::code);
    ASSERT_NE(issue, validation.issues.end());
    EXPECT_NE(issue->message.find("1 cluster(s) are referenced by index records but marked free"), std::string::npos);
}

TEST(SfsReader, ReportsMissingDirectoryTargetsAndChildCycles) {
    auto missing_reader = std::make_shared<axk::MemoryReader>(image_fixture());
    const auto missing = axk::open_image(missing_reader, "missing-link.hds");
    ASSERT_TRUE(missing);
    const auto &missing_diagnostics = missing->partitions()[0].diagnostics;
    EXPECT_TRUE(std::any_of(missing_diagnostics.begin(), missing_diagnostics.end(), [](const axk::Error &error) {
        return error.code == axk::ErrorCode::relationship_unresolved && error.context.object_name == "Samples";
    }));

    auto cycle_bytes = image_fixture();
    axk::ByteWriter writer{cycle_bytes};
    const auto directory = 15U * sector_size;
    ASSERT_TRUE(writer.write_be32(directory + 64U + 4U, 0));
    auto cycle_reader = std::make_shared<axk::MemoryReader>(std::move(cycle_bytes));
    const auto cycle = axk::open_image(cycle_reader, "directory-cycle.hds");
    ASSERT_TRUE(cycle);
    const auto &cycle_diagnostics = cycle->partitions()[0].diagnostics;
    EXPECT_TRUE(std::any_of(cycle_diagnostics.begin(), cycle_diagnostics.end(), [](const axk::Error &error) {
        return error.code == axk::ErrorCode::relationship_cycle && error.context.object_name == "Samples";
    }));
}

TEST(SfsReader, RejectsNonSfsAndCancellationWithoutPartialContainer) {
    auto bytes = image_fixture();
    std::fill_n(bytes.begin(), 11, std::byte{});
    auto invalid = std::make_shared<axk::MemoryReader>(std::move(bytes));
    const auto rejected = axk::open_image(invalid, "invalid.hds");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, axk::ErrorCode::container_unrecognized);

    axk::OpenOptions options;
    axk::CancellationSource source;
    source.cancel();
    options.cancellation = source.token();
    auto valid = std::make_shared<axk::MemoryReader>(image_fixture());
    const auto cancelled = axk::open_image(valid, "fixture.hds", options);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
}
