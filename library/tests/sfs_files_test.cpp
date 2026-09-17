#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/filesystem_edit.hpp"
#include "axklib/filesystem_import.hpp"
#include "axklib/filesystem_transaction.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"
#include "sfs_cluster_allocation.hpp"

namespace {
std::optional<std::string> external_sfs_image() {
#if defined(_WIN32)
    char *raw = nullptr;
    std::size_t size{};
    if (_dupenv_s(&raw, &size, "AXK_TEST_SFS_IMAGE") != 0 || raw == nullptr)
        return std::nullopt;
    const std::unique_ptr<char, decltype(&std::free)> value{raw, &std::free};
    return *value == '\0' ? std::nullopt : std::optional<std::string>{value.get()};
#else
    const auto *value = std::getenv("AXK_TEST_SFS_IMAGE");
    return value == nullptr || *value == '\0' ? std::nullopt : std::optional<std::string>{value};
#endif
}

class SfsFiles : public testing::Test {
  protected:
    std::filesystem::path folder;
    std::filesystem::path source;
    void SetUp() override {
        folder = std::filesystem::temp_directory_path() /
                 std::format("axk-raw-files-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directory(folder);
        source = folder / "source.hds";
        const axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {{"Files", {}}}};
        ASSERT_TRUE(axk::write_hds_image(manifest, source));
        const auto image = axk::open_image(source);
        ASSERT_TRUE(image);
        EXPECT_TRUE(image->backup_superblock_matches());
        EXPECT_TRUE(image->partitions().front().backup_header_matches);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }
    static std::shared_ptr<axk::MemoryReader> data(std::size_t size, std::byte value = std::byte{0x5a}) {
        return std::make_shared<axk::MemoryReader>(std::vector<std::byte>(size, value));
    }
    static axk::package_internal::Sha256Digest digest(const std::filesystem::path &path) {
        const auto reader = axk::FileReader::open(path);
        EXPECT_TRUE(reader);
        return axk::package_internal::sha256_reader(**reader).value();
    }
    static const axk::IndexRecord &named(const axk::Container &image, std::string_view name) {
        const auto &partition = image.partitions().front();
        for (const auto &directory : partition.records) {
            const auto entry = std::ranges::find(directory.directory_entries, name, &axk::DirectoryEntry::name);
            if (entry != directory.directory_entries.end())
                return *std::ranges::find(partition.records, axk::SfsId{entry->raw_link_id.value},
                                          &axk::IndexRecord::sfs_id);
        }
        ADD_FAILURE() << "missing file " << name;
        return partition.records.front();
    }
    static void patch(const std::filesystem::path &path, std::uint64_t offset, std::span<const std::byte> bytes) {
        std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(stream);
        stream.seekp(static_cast<std::streamoff>(offset));
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(stream);
    }
    static void promote_large(const std::filesystem::path &path, std::string_view name, std::uint32_t unit) {
        const auto image = axk::open_image(path).value();
        const auto &partition = image.partitions().front();
        const auto &record = named(image, name);
        ASSERT_TRUE(record.continuation_clusters.empty());
        const auto payload = image.read_record_data(partition.index, record.sfs_id, 65536U).value();
        const auto cluster_bytes = partition.sectors_per_cluster * image.superblock().sector_size_bytes;
        const auto base = static_cast<std::uint64_t>(partition.start_sector) * image.superblock().sector_size_bytes;
        const auto reader = axk::FileReader::open(path).value();
        std::vector<std::byte> bytes(static_cast<std::size_t>(reader->size()));
        ASSERT_TRUE(reader->read_exact_at(0U, bytes));
        axk::ByteWriter writer{bytes};
        ASSERT_TRUE(writer.write_be32(base + 0x84U, unit));
        ASSERT_TRUE(writer.write_be32(base + cluster_bytes + 0x84U, unit));
        const auto mark = [&](std::uint32_t cluster, bool allocated) {
            for (const auto bitmap : {partition.bitmap_copy1_cluster, partition.bitmap_copy2_cluster}) {
                auto &byte = bytes[base + static_cast<std::uint64_t>(bitmap) * cluster_bytes + cluster / 8U];
                const auto mask = static_cast<std::byte>(0x80U >> (cluster % 8U));
                byte = allocated ? byte | mask : byte & ~mask;
            }
        };
        for (const auto &extent : record.extents)
            for (std::uint32_t offset = 0; offset < extent.cluster_count; ++offset)
                mark(extent.cluster_offset + offset, false);
        const auto count = payload.empty() ? 0U : ((record.data_size - 1U) / (unit * cluster_bytes) + 1U) * unit;
        auto start = partition.directory_index_cluster + partition.directory_index_span_clusters;
        start = ((start + unit - 1U) / unit) * unit;
        for (; count != 0U && start + count <= partition.cluster_count; start += unit) {
            bool free = true;
            for (std::uint32_t offset = 0; offset < count; ++offset) {
                const auto cluster = start + offset;
                const auto byte =
                    bytes[base + static_cast<std::uint64_t>(partition.bitmap_copy1_cluster) * cluster_bytes +
                          cluster / 8U];
                free = free && (byte & static_cast<std::byte>(0x80U >> (cluster % 8U))) == std::byte{};
            }
            if (free)
                break;
        }
        ASSERT_LE(start + count, partition.cluster_count);
        for (std::uint32_t offset = 0; offset < count; ++offset)
            mark(start + offset, true);
        const auto index = record.record_offset.value;
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(index + 0x0aU), 48U, std::byte{});
        ASSERT_TRUE(writer.write_be16(index, count == 0U ? 0U : 1U));
        ASSERT_TRUE(writer.write_be16(index + 4U, static_cast<std::uint16_t>(count)));
        ASSERT_TRUE(writer.write_be32(index + 0x42U, record.attributes | 0x20000000U));
        if (count != 0U) {
            ASSERT_TRUE(writer.write_be32(index + 0x0aU, start));
            ASSERT_TRUE(writer.write_be32(index + 0x0eU, count));
            ASSERT_TRUE(writer.write_be32(index + 0x12U, record.data_size));
            std::copy(payload.begin(), payload.end(),
                      bytes.begin() +
                          static_cast<std::ptrdiff_t>(base + static_cast<std::uint64_t>(start) * cluster_bytes));
        }
        patch(path, 0U, bytes);
        EXPECT_TRUE(axk::allocation_is_safe_for_mutation(axk::open_image(path)->partitions().front().allocation));
    }
};

TEST(SfsClusterAllocation, SelectsWholeUnitsAndPreservesOrdinarySelection) {
    const auto available = [](std::uint32_t cluster) { return cluster == 9U || cluster == 20U; };
    EXPECT_EQ(axk::detail::select_sfs_payload_clusters(6U, 31U, 8U, available, 4U),
              (std::vector<std::uint32_t>{12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U}));
    const auto fragmented = [](std::uint32_t cluster) { return cluster >= 12U && cluster < 16U; };
    EXPECT_EQ(axk::detail::select_sfs_payload_clusters(6U, 23U, 8U, fragmented, 4U),
              (std::vector<std::uint32_t>{8U, 9U, 10U, 11U, 16U, 17U, 18U, 19U}));
    EXPECT_EQ(axk::detail::select_sfs_payload_clusters(6U, 23U, 8U, fragmented),
              (std::vector<std::uint32_t>{6U, 7U, 8U, 9U, 10U, 11U, 16U, 17U}));
    EXPECT_FALSE(axk::detail::select_sfs_payload_clusters(6U, 23U, 12U, fragmented, 4U));
    EXPECT_FALSE(axk::detail::select_sfs_payload_clusters(
        8U, 16U, 4U, [](std::uint32_t cluster) { return cluster == 9U || cluster == 13U; }, 4U));
}

TEST(SfsClusterAllocation, RejectsInvalidUnitsCountsAndOverflowingBounds) {
    const auto unused = [](std::uint32_t) { return false; };
    EXPECT_FALSE(axk::detail::select_sfs_payload_clusters(0U, 20U, 1U, unused, 0U));
    EXPECT_FALSE(axk::detail::select_sfs_payload_clusters(0U, 20U, 5U, unused, 4U));
    EXPECT_FALSE(axk::detail::select_sfs_payload_clusters(20U, 10U, 4U, unused, 4U));
    constexpr auto end = std::numeric_limits<std::uint32_t>::max();
    EXPECT_FALSE(axk::detail::select_sfs_payload_clusters(end - 2U, end, 4U, unused, 4U));
    EXPECT_EQ(axk::detail::select_sfs_payload_clusters(end - 7U, end, 4U, unused, 4U),
              (std::vector<std::uint32_t>{end - 7U, end - 6U, end - 5U, end - 4U}));
    EXPECT_EQ(axk::detail::select_sfs_payload_clusters(0U, 20U, 0U, unused, 4U), std::vector<std::uint32_t>{});
}

TEST_F(SfsFiles, RenamesEntriesInPlaceWithoutChangingIndexRecordsOrAllocation) {
    const auto populated = folder / "populated.hds";
    const std::vector<axk::FilesystemEdit> initial{
        axk::CreateFilesystemDirectory{{"Documents"}},
        axk::PutFilesystemFile{{"Documents", "payload.bin"}, data(8193)},
    };
    ASSERT_TRUE(axk::write_sfs_file_edits(source, populated, axk::PartitionIndex{0}, initial));
    const auto reader = axk::FileReader::open(populated).value();
    const auto original = axk::open_image(populated).value();
    const auto root = axk::locate_partition_root_record(original.partitions().front()).value();
    const auto &root_record =
        *std::ranges::find(original.partitions().front().records, root, &axk::IndexRecord::sfs_id);
    const auto &directory = named(original, "Documents");
    const std::vector<axk::FilesystemEdit> edits{
        axk::RenameFilesystemEntry{{"Documents"}, "Renamed"},
        axk::RenameFilesystemEntry{{"Renamed", "payload.bin"}, "short"},
    };
    const auto plan = axk::detail::prepare_sfs_file_edits(reader, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    const auto changed = axk::open_image(plan->preview, {}).value();
    EXPECT_EQ(named(changed, "Renamed").sfs_id, directory.sfs_id);
    EXPECT_EQ(named(changed, "short").sfs_id, named(original, "payload.bin").sfs_id);
    const auto in_directory = [&](std::uint64_t offset) {
        for (const auto *record : {&root_record, &directory})
            for (const auto &extent : record->extents) {
                const auto &partition = original.partitions().front();
                const auto begin =
                    static_cast<std::uint64_t>(partition.start_sector) * original.superblock().sector_size_bytes +
                    static_cast<std::uint64_t>(extent.cluster_offset) * partition.sectors_per_cluster *
                        original.superblock().sector_size_bytes;
                if (offset >= begin && offset < begin + extent.byte_count)
                    return true;
            }
        return false;
    };
    std::vector<std::byte> before(static_cast<std::size_t>(reader->size()));
    auto after = before;
    ASSERT_TRUE(reader->read_exact_at(0U, before));
    ASSERT_TRUE(plan->preview->read_exact_at(0U, after));
    for (std::size_t i = 0; i < before.size(); ++i)
        if (before[i] != after[i])
            ASSERT_TRUE(in_directory(i)) << i;
    EXPECT_EQ(*changed.read_record_data(axk::PartitionIndex{0}, named(changed, "short").sfs_id, 16384U),
              std::vector<std::byte>(8193U, std::byte{0x5a}));
    for (const auto &name : {"Renamed", "sfserrlog", "..", "a/b", "", "abcdefghijklmnopqrstuvwxyz"}) {
        const std::vector<axk::FilesystemEdit> invalid{axk::RenameFilesystemEntry{{"Renamed"}, name}};
        EXPECT_FALSE(axk::detail::prepare_sfs_file_edits(plan->preview, axk::PartitionIndex{0}, invalid)) << name;
    }
    const std::vector<axk::FilesystemEdit> collision{axk::CreateFilesystemDirectory{{"Taken"}},
                                                     axk::RenameFilesystemEntry{{"Renamed"}, "Taken"}};
    EXPECT_FALSE(axk::detail::prepare_sfs_file_edits(plan->preview, axk::PartitionIndex{0}, collision));
}

TEST_F(SfsFiles, CreatesEmptyDirectoriesAndExactRawFilesWithoutSamplerCategories) {
    const auto before = digest(source);
    const std::vector<axk::FilesystemEdit> edits{
        axk::CreateFilesystemDirectory{{"Documents"}},
        axk::PutFilesystemFile{{"Documents", "empty.bin"}, data(0)},
        axk::PutFilesystemFile{{"Documents", "payload.bin"}, data(8193)},
    };
    const auto output = folder / "edited.hds";
    const auto written = axk::write_sfs_file_edits(source, output, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(digest(source), before);
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image) << image.error().message;
    const auto &partition = image->partitions().front();
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(partition.allocation));
    EXPECT_TRUE(partition.diagnostics.empty());
    const auto root = axk::locate_partition_root_record(partition).value();
    const auto &root_record = *std::ranges::find(partition.records, root, &axk::IndexRecord::sfs_id);
    EXPECT_EQ(root_record.link_count, 3U);
    const auto entry = std::ranges::find(root_record.directory_entries, "Documents", &axk::DirectoryEntry::name);
    ASSERT_NE(entry, root_record.directory_entries.end());
    const auto &directory =
        *std::ranges::find(partition.records, axk::SfsId{entry->raw_link_id.value}, &axk::IndexRecord::sfs_id);
    EXPECT_EQ(directory.link_count, 2U);
    EXPECT_EQ(directory.attributes, 0x94646972U);
    EXPECT_EQ(named(*image, "sfserram").attributes, 0x94000000U);
    ASSERT_EQ(directory.directory_entries.size(), 4U);
    for (const auto &[name, size] :
         std::vector<std::pair<std::string, std::size_t>>{{"empty.bin", 0}, {"payload.bin", 8193}}) {
        const auto file = std::ranges::find(directory.directory_entries, name, &axk::DirectoryEntry::name);
        ASSERT_NE(file, directory.directory_entries.end());
        const auto id = axk::SfsId{file->raw_link_id.value};
        const auto &record = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
        EXPECT_EQ(record.data_size, size);
        EXPECT_EQ(record.link_count, 1U);
        EXPECT_EQ(record.attributes, 0x9e000000U);
        const auto bytes = image->read_record_data(partition.index, id, 16384U);
        ASSERT_TRUE(bytes);
        EXPECT_EQ(*bytes, std::vector<std::byte>(size, std::byte{0x5a}));
        if (size == 0) {
            EXPECT_TRUE(record.extents.empty());
        }
    }
}

TEST_F(SfsFiles, ReplacesLargeUnitFilesWithAlignedCapacityAndPreservedFlags) {
    for (const auto unit : {1U, 4U, 200U}) {
        SCOPED_TRACE(unit);
        auto current = folder / std::format("large-{}.hds", unit);
        const std::vector<axk::FilesystemEdit> initial{
            axk::PutFilesystemFile{{"large"}, data(0)},
            axk::PutFilesystemFile{{"untouched"}, data(1234)},
        };
        ASSERT_TRUE(axk::write_sfs_file_edits(source, current, axk::PartitionIndex{0}, initial));
        const auto original = axk::open_image(current).value();
        const auto &partition = original.partitions().front();
        const auto cluster_bytes = partition.sectors_per_cluster * original.superblock().sector_size_bytes;
        const auto start = static_cast<std::uint64_t>(partition.start_sector) * original.superblock().sector_size_bytes;
        std::array<std::byte, 4> word{};
        ASSERT_TRUE(axk::ByteWriter{word}.write_be32(0U, unit));
        patch(current, start + 0x84U, word);
        patch(current, start + cluster_bytes + 0x84U, word);
        ASSERT_TRUE(axk::ByteWriter{word}.write_be32(0U, 0xfe000000U));
        patch(current, named(original, "large").record_offset.value + 0x42U, word);
        std::size_t step{};
        for (const auto size : {1U, unit * cluster_bytes + 1U, 123U, 0U}) {
            const auto before = digest(current);
            const auto output = folder / std::format("large-{}-{}.hds", unit, step++);
            const std::vector<axk::FilesystemEdit> edits{
                axk::PutFilesystemFile{{"large"}, data(size), axk::FileConflict::replace},
            };
            const auto written = axk::write_sfs_file_edits(current, output, axk::PartitionIndex{0}, edits);
            ASSERT_TRUE(written) << written.error().message;
            EXPECT_EQ(digest(current), before);
            const auto image = axk::open_image(output).value();
            EXPECT_EQ(image.partitions().front().large_allocation_unit_clusters, unit);
            const auto &record = named(image, "large");
            EXPECT_EQ(record.attributes, 0xfe000000U);
            EXPECT_EQ(record.data_size, size);
            const auto expected_clusters = size == 0U ? 0U : ((size - 1U) / (unit * cluster_bytes) + 1U) * unit;
            EXPECT_EQ(record.cluster_count, expected_clusters);
            for (const auto &extent : record.extents) {
                EXPECT_EQ(extent.cluster_offset % unit, 0U);
                EXPECT_EQ(extent.cluster_count % unit, 0U);
            }
            EXPECT_TRUE(axk::allocation_is_safe_for_mutation(image.partitions().front().allocation));
            EXPECT_EQ(*image.read_record_data(axk::PartitionIndex{0}, record.sfs_id, 1024U * 1024U),
                      std::vector<std::byte>(size, std::byte{0x5a}));
            EXPECT_EQ(named(image, "untouched").record_offset, named(original, "untouched").record_offset);
            EXPECT_EQ(*image.read_record_data(axk::PartitionIndex{0}, named(image, "untouched").sfs_id, 1234U),
                      std::vector<std::byte>(1234U, std::byte{0x5a}));
            current = output;
        }
    }
}

TEST_F(SfsFiles, ReviewsImportPathsWithDirectoryMergingAndExplicitFileConflictsWithoutWriting) {
    const auto populated = folder / "review.hds";
    const std::vector<axk::FilesystemEdit> initial{axk::CreateFilesystemDirectory{{"Folder"}},
                                                   axk::PutFilesystemFile{{"Folder", "existing"}, data(17)}};
    ASSERT_TRUE(axk::write_sfs_file_edits(source, populated, axk::PartitionIndex{0}, initial));
    const auto before = digest(populated);
    const auto reader = axk::FileReader::open(populated);
    ASSERT_TRUE(reader);
    using Action = axk::FilesystemImportAction;
    const std::vector<axk::FilesystemImportEntry> inputs{
        {{"Folder"}, true},
        {{"Folder", "existing"}, false, 42},
        {{"Folder", "Empty"}, true},
        {{"Folder", "Empty", "new"}, false, 0},
        {{"Folder", "EXISTING"}, false, 7},
    };
    const auto review = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, inputs);
    ASSERT_TRUE(review) << review.error().message;
    ASSERT_EQ(review->size(), inputs.size());
    EXPECT_EQ((*review)[0].action, Action::merge_directory);
    EXPECT_EQ((*review)[1].action, Action::skip_file);
    EXPECT_EQ((*review)[1].existing_size_bytes, 17U);
    EXPECT_EQ((*review)[2].action, Action::create_directory);
    EXPECT_EQ((*review)[3].action, Action::create_file);
    EXPECT_EQ((*review)[4].action, Action::create_file);
    auto replace = inputs;
    replace[1].conflict = axk::FileConflict::replace;
    const auto replaced = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, replace);
    ASSERT_TRUE(replaced);
    EXPECT_EQ((*replaced)[1].action, Action::replace_file);
    std::vector<axk::FilesystemEdit> edits;
    for (const auto &entry : replace) {
        if (entry.directory)
            edits.emplace_back(axk::CreateFilesystemDirectory{entry.path});
        else
            edits.emplace_back(
                axk::PutFilesystemFile{entry.path, data(static_cast<std::size_t>(entry.size_bytes)), entry.conflict});
    }
    const auto output = folder / "reviewed-write.hds";
    ASSERT_TRUE(axk::write_sfs_file_edits(populated, output, axk::PartitionIndex{0}, edits));
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(named(*reopened, "existing").data_size, 42U);
    EXPECT_EQ(named(*reopened, "EXISTING").data_size, 7U);
    EXPECT_EQ(named(*reopened, "new").data_size, 0U);
    EXPECT_EQ(digest(populated), before);
}

TEST_F(SfsFiles, ImportReviewReportsEveryBadPathAndIncomingCollision) {
    const auto populated = folder / "conflicts.hds";
    const std::vector<axk::FilesystemEdit> initial{axk::CreateFilesystemDirectory{{"Directory"}},
                                                   axk::PutFilesystemFile{{"file"}, data(1)}};
    ASSERT_TRUE(axk::write_sfs_file_edits(source, populated, axk::PartitionIndex{0}, initial));
    const auto reader = axk::FileReader::open(populated);
    ASSERT_TRUE(reader);
    const std::vector<axk::FilesystemImportEntry> inputs{
        {{"Directory"}, false, 1},
        {{"file"}, true},
        {{"file", "child"}, false, 1},
        {{"missing", "child"}, false, 1},
        {{"sfserram"}, false, 1},
        {{".."}, true},
        {{"slash/name"}, false, 1},
        {{std::string(24, 'x')}, false, 1},
        {{"same"}, false, 1},
        {{"same"}, false, 2, axk::FileConflict::replace},
        {{"empty"}, true},
        {{"empty"}, true},
    };
    const auto review = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, inputs);
    ASSERT_TRUE(review) << review.error().message;
    ASSERT_EQ(review->size(), inputs.size());
    for (std::size_t i = 0; i < 8U; ++i) {
        EXPECT_EQ((*review)[i].action, axk::FilesystemImportAction::conflict) << i;
        EXPECT_FALSE((*review)[i].issue.empty()) << i;
    }
    EXPECT_EQ((*review)[8].action, axk::FilesystemImportAction::create_file);
    EXPECT_EQ((*review)[9].action, axk::FilesystemImportAction::replace_file);
    EXPECT_EQ((*review)[9].existing_size_bytes, 1U);
    EXPECT_FALSE((*review)[9].issue.empty());
    EXPECT_EQ((*review)[10].action, axk::FilesystemImportAction::create_directory);
    EXPECT_EQ((*review)[11].action, axk::FilesystemImportAction::merge_directory);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, inputs, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
}

TEST_F(SfsFiles, ImportReviewDetectsDuplicateDestinationsThroughDirectoryAliases) {
    const auto aliases = folder / "directory-alias.hds";
    const std::vector<axk::FilesystemEdit> initial{axk::CreateFilesystemDirectory{{"Folder"}},
                                                   axk::PutFilesystemFile{{"Alias"}, data(0)}};
    ASSERT_TRUE(axk::write_sfs_file_edits(source, aliases, axk::PartitionIndex{0}, initial));
    std::uint64_t alias_index{}, directory_index{}, alias_link{};
    axk::SfsId directory_id;
    {
        const auto image = axk::open_image(aliases);
        ASSERT_TRUE(image);
        const auto &directory = named(*image, "Folder");
        directory_id = directory.sfs_id;
        directory_index = directory.record_offset.value;
        alias_index = named(*image, "Alias").record_offset.value;
        const auto &partition = image->partitions().front();
        const auto root_id = axk::locate_partition_root_record(partition).value();
        const auto &root = *std::ranges::find(partition.records, root_id, &axk::IndexRecord::sfs_id);
        const auto &entry = *std::ranges::find(root.directory_entries, "Alias", &axk::DirectoryEntry::name);
        alias_link = static_cast<std::uint64_t>(partition.start_sector) * 512U +
                     static_cast<std::uint64_t>(root.extents.front().cluster_offset) * 1024U +
                     entry.payload_relative_offset + 4U;
    }
    std::array<std::byte, 4> link{};
    ASSERT_TRUE(axk::ByteWriter{link}.write_be32(0U, directory_id.value));
    patch(aliases, alias_link, link);
    patch(aliases, alias_index, std::array<std::byte, 72>{});
    patch(aliases, directory_index + 0x46U, std::array<std::byte, 2>{std::byte{}, std::byte{3}});
    const auto before = digest(aliases);
    const auto reader = axk::FileReader::open(aliases);
    ASSERT_TRUE(reader);
    const std::vector<axk::FilesystemImportEntry> inputs{
        {{"Folder", "new"}, false, 12},
        {{"Alias", "new"}, false, 7},
        {{"Alias", "new"}, false, 20, axk::FileConflict::replace},
        {{"Folder", "new"}, false, 4},
    };
    const auto review = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, inputs);
    ASSERT_TRUE(review) << review.error().message;
    EXPECT_EQ(review->at(0).action, axk::FilesystemImportAction::create_file);
    EXPECT_EQ(review->at(1).action, axk::FilesystemImportAction::skip_file);
    EXPECT_EQ(review->at(1).existing_size_bytes, 12U);
    EXPECT_EQ(review->at(2).action, axk::FilesystemImportAction::replace_file);
    EXPECT_EQ(review->at(2).existing_size_bytes, 12U);
    EXPECT_EQ(review->at(3).action, axk::FilesystemImportAction::skip_file);
    EXPECT_EQ(review->at(3).existing_size_bytes, 20U);
    EXPECT_EQ(digest(aliases), before);
    const auto protected_image = folder / "protected-alias.hds";
    std::filesystem::copy_file(aliases, protected_image);
    std::uint64_t protected_entry_offset{};
    std::array<std::byte, 30> support_entry{};
    {
        const auto image = axk::open_image(protected_image);
        ASSERT_TRUE(image);
        const auto &partition = image->partitions().front();
        const auto root_id = axk::locate_partition_root_record(partition).value();
        const auto &root = *std::ranges::find(partition.records, root_id, &axk::IndexRecord::sfs_id);
        const auto &entry = *std::ranges::find(root.directory_entries, "Folder", &axk::DirectoryEntry::name);
        const auto offset = static_cast<std::uint64_t>(partition.start_sector) * 512U +
                            static_cast<std::uint64_t>(root.extents.front().cluster_offset) * 1024U +
                            entry.payload_relative_offset;
        axk::ByteWriter writer{support_entry};
        ASSERT_TRUE(writer.write_be16(0U, 5U));
        ASSERT_TRUE(writer.write_be32(2U, directory_id.value));
        support_entry[6] = std::byte{'P'};
        support_entry[7] = std::byte{'R'};
        support_entry[8] = std::byte{'F'};
        support_entry[9] = std::byte{'3'};
        protected_entry_offset = offset + 2U;
    }
    patch(protected_image, protected_entry_offset, support_entry);
    const auto protected_reader = axk::FileReader::open(protected_image);
    ASSERT_TRUE(protected_reader);
    const std::vector<axk::FilesystemImportEntry> protected_input{{{"Alias", "new"}, false, 1U}};
    const auto protected_review =
        axk::inspect_sfs_file_import(*protected_reader, axk::PartitionIndex{0}, protected_input);
    ASSERT_TRUE(protected_review) << protected_review.error().message;
    EXPECT_EQ(protected_review->front().action, axk::FilesystemImportAction::conflict);
    EXPECT_NE(protected_review->front().issue.find("metadata is protected"), std::string::npos);
}

TEST_F(SfsFiles, ImportReviewBoundsCountsSizesAndDepthWithoutReadingInputPayloads) {
    const auto reader = axk::FileReader::open(source);
    ASSERT_TRUE(reader);
    const std::vector<axk::FilesystemImportEntry> invalid{
        {{"too-large"}, false, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U},
        {{"directory-with-size"}, true, 1U},
        {axk::FilesystemPath(64U, "deep"), false, 0U},
        {{"policy"}, false, 0U, static_cast<axk::FileConflict>(99)},
    };
    const auto rejected = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, invalid);
    ASSERT_TRUE(rejected);
    for (const auto &row : *rejected)
        EXPECT_EQ(row.action, axk::FilesystemImportAction::conflict);
    std::vector<axk::FilesystemImportEntry> entries;
    for (std::size_t i = 0; i < 10000U; ++i)
        entries.push_back({{std::format("file{}", i)}, false, 0U});
    const auto reviewed = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, entries);
    ASSERT_TRUE(reviewed) << reviewed.error().message;
    EXPECT_EQ(reviewed->size(), 10000U);
    EXPECT_TRUE(std::ranges::all_of(
        *reviewed, [](const auto &row) { return row.action == axk::FilesystemImportAction::create_file; }));
    entries.push_back({{"overflow"}, false, 0U});
    EXPECT_FALSE(axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, entries));
    EXPECT_FALSE(axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, {}));
}

TEST_F(SfsFiles, PreparesNonoverlappingReaderBackedPatchesWithoutWritingTheSource) {
    const auto before = digest(source);
    auto reader = axk::FileReader::open(source);
    ASSERT_TRUE(reader);
    const std::vector<axk::FilesystemEdit> edits{
        axk::CreateFilesystemDirectory{{"Raw"}},
        axk::PutFilesystemFile{{"Raw", "one"}, data(12001U, std::byte{0x11})},
        axk::PutFilesystemFile{{"Raw", "one"}, data(9001U, std::byte{0x22}), axk::FileConflict::replace},
        axk::PutFilesystemFile{{"Raw", "two"}, data(3001U, std::byte{0x33})},
        axk::RemoveFilesystemEntry{{"Raw", "one"}},
        axk::PutFilesystemFile{{"Raw", "three"}, data(17001U, std::byte{0x44})},
    };
    const auto prepared = axk::detail::prepare_sfs_file_edits(*reader, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(digest(source), before);
    EXPECT_EQ(prepared->image_size_bytes, (*reader)->size());
    std::uint64_t end{};
    ASSERT_FALSE(prepared->patches.empty());
    for (const auto &range : prepared->patches) {
        EXPECT_GE(range.offset, end);
        end = range.offset + range.size;
        EXPECT_LE(end, (*reader)->size());
        ASSERT_TRUE(range.source);
        EXPECT_LE(range.source_offset + range.size, range.source->size());
    }
    const auto preview = axk::open_image(prepared->preview, {});
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(preview->partitions().front().allocation));
    EXPECT_EQ(named(*preview, "three").data_size, 17001U);
    const auto output = folder / "published.hds";
    ASSERT_TRUE(axk::write_sfs_file_edits(source, output, axk::PartitionIndex{0}, edits));
    EXPECT_EQ(axk::package_internal::sha256_reader(*prepared->preview).value(), digest(output));
}

TEST_F(SfsFiles, RequiresRecursiveConfirmationAndPreservesSourceOnFailure) {
    const std::vector<axk::FilesystemEdit> initial{axk::CreateFilesystemDirectory{{"Folder"}},
                                                   axk::PutFilesystemFile{{"Folder", "data"}, data(4097)}};
    const auto populated = folder / "populated.hds";
    ASSERT_TRUE(axk::write_sfs_file_edits(source, populated, axk::PartitionIndex{0}, initial));
    const auto before = digest(populated);
    const auto output = folder / "deleted.hds";
    const std::vector<axk::FilesystemEdit> unconfirmed{axk::RemoveFilesystemEntry{{"Folder"}, false}};
    EXPECT_FALSE(axk::write_sfs_file_edits(populated, output, axk::PartitionIndex{0}, unconfirmed));
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(digest(populated), before);
    const std::vector<axk::FilesystemEdit> confirmed{axk::RemoveFilesystemEntry{{"Folder"}, true}};
    const auto removed = axk::write_sfs_file_edits(populated, output, axk::PartitionIndex{0}, confirmed);
    ASSERT_TRUE(removed) << removed.error().message;
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(image->partitions().front().allocation));
    EXPECT_EQ(image->partitions().front().records.size(), axk::open_image(source)->partitions().front().records.size());
}

TEST_F(SfsFiles, SkipsExistingFilesUnlessReplaceIsExplicitAndMergesDirectories) {
    const std::vector<axk::FilesystemEdit> initial{axk::CreateFilesystemDirectory{{"Folder"}},
                                                   axk::PutFilesystemFile{{"Folder", "data"}, data(4097)}};
    const auto populated = folder / "populated.hds";
    ASSERT_TRUE(axk::write_sfs_file_edits(source, populated, axk::PartitionIndex{0}, initial));
    const std::vector<axk::FilesystemEdit> skip{axk::CreateFilesystemDirectory{{"Folder"}},
                                                axk::PutFilesystemFile{{"Folder", "data"}, data(12)}};
    const auto skipped = folder / "skipped.hds";
    ASSERT_TRUE(axk::write_sfs_file_edits(populated, skipped, axk::PartitionIndex{0}, skip));
    EXPECT_EQ(digest(populated), digest(skipped));
    const std::vector<axk::FilesystemEdit> replace{
        axk::PutFilesystemFile{{"Folder", "data"}, data(12), axk::FileConflict::replace}};
    const auto replaced = folder / "replaced.hds";
    const auto result = axk::write_sfs_file_edits(populated, replaced, axk::PartitionIndex{0}, replace);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_NE(digest(populated), digest(replaced));
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(axk::open_image(replaced)->partitions().front().allocation));
}

TEST_F(SfsFiles, RejectsProtectedEntriesInvalidPathsCollisionsAndCancelledPublication) {
    const auto before = digest(source);
    for (const auto &edit : std::vector<axk::FilesystemEdit>{
             axk::RemoveFilesystemEntry{{"sfserrlog"}, true}, axk::RemoveFilesystemEntry{{}, true},
             axk::CreateFilesystemDirectory{{".."}}, axk::PutFilesystemFile{{"bad/name"}, data(1)}}) {
        const std::array edits{edit};
        EXPECT_FALSE(axk::write_sfs_file_edits(source, folder / "bad.hds", axk::PartitionIndex{0}, edits));
        EXPECT_FALSE(std::filesystem::exists(folder / "bad.hds"));
    }
    const std::vector<axk::FilesystemEdit> collision{axk::CreateFilesystemDirectory{{"same"}},
                                                     axk::PutFilesystemFile{{"same"}, data(1)}};
    EXPECT_FALSE(axk::write_sfs_file_edits(source, folder / "bad.hds", axk::PartitionIndex{0}, collision));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(
        axk::write_sfs_file_edits(source, folder / "bad.hds", axk::PartitionIndex{0}, collision, cancellation.token()));
    EXPECT_FALSE(std::filesystem::exists(folder / "bad.hds"));
    EXPECT_EQ(digest(source), before);
}

TEST_F(SfsFiles, GrowsDirectoriesAndUsesContinuationExtentsWithoutChangingOtherFiles) {
    std::vector<axk::FilesystemEdit> initial;
    for (unsigned i = 0; i < 70; ++i)
        initial.push_back(axk::PutFilesystemFile{{std::format("file{:03}", i)}, data(2048, static_cast<std::byte>(i))});
    const auto populated = folder / "populated.hds";
    const auto populated_result = axk::write_sfs_file_edits(source, populated, axk::PartitionIndex{0}, initial);
    ASSERT_TRUE(populated_result) << populated_result.error().message;
    const auto free_clusters =
        axk::open_image(populated)->partitions().front().allocation.free_space->free_cluster_count;
    const std::vector<axk::FilesystemEdit> fill{
        axk::PutFilesystemFile{{"filler"}, data(static_cast<std::size_t>(free_clusters - 4U) * 1024U)}};
    const auto packed = folder / "packed.hds";
    const auto packed_result = axk::write_sfs_file_edits(populated, packed, axk::PartitionIndex{0}, fill);
    ASSERT_TRUE(packed_result) << packed_result.error().message;
    std::vector<axk::FilesystemEdit> changes;
    for (unsigned i = 0; i < 70; i += 2)
        changes.push_back(axk::RemoveFilesystemEntry{{std::format("file{:03}", i)}, false});
    changes.push_back(axk::PutFilesystemFile{{"fragmented"}, data(20U * 1024U)});
    const auto output = folder / "fragmented.hds";
    const auto changed = axk::write_sfs_file_edits(packed, output, axk::PartitionIndex{0}, changes);
    ASSERT_TRUE(changed) << changed.error().message;
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    const auto &record = named(*image, "fragmented");
    EXPECT_GT(record.extents.size(), 4U);
    EXPECT_FALSE(record.continuation_clusters.empty());
    EXPECT_EQ(*image->read_record_data(axk::PartitionIndex{0}, record.sfs_id, 32768U),
              std::vector<std::byte>(20U * 1024U, std::byte{0x5a}));
    const auto previous = axk::open_image(populated);
    ASSERT_TRUE(previous);
    for (unsigned i = 1; i < 70; i += 2) {
        const auto name = std::format("file{:03}", i);
        const auto &unchanged = named(*image, name);
        const auto &original = named(*previous, name);
        EXPECT_EQ(unchanged.record_offset, original.record_offset);
        EXPECT_EQ(unchanged.attributes, original.attributes);
        EXPECT_EQ(unchanged.extents.front().cluster_offset, original.extents.front().cluster_offset);
        EXPECT_EQ(*image->read_record_data(axk::PartitionIndex{0}, unchanged.sfs_id, 4096U),
                  std::vector<std::byte>(2048U, static_cast<std::byte>(i)));
    }
}

TEST_F(SfsFiles, PreservesAliasDataAndNativeAttributesWhenReplacingOneName) {
    const auto aliases = folder / "aliases.hds";
    const std::vector<axk::FilesystemEdit> initial{axk::PutFilesystemFile{{"first"}, data(4096)},
                                                   axk::PutFilesystemFile{{"second"}, data(0)}};
    ASSERT_TRUE(axk::write_sfs_file_edits(source, aliases, axk::PartitionIndex{0}, initial));
    ASSERT_NO_FATAL_FAILURE(promote_large(aliases, "first", 4U));
    std::uint64_t first_index{};
    std::uint64_t second_index{};
    std::uint64_t second_link{};
    axk::SfsId first_id;
    {
        const auto image = axk::open_image(aliases);
        ASSERT_TRUE(image);
        first_index = named(*image, "first").record_offset.value;
        second_index = named(*image, "second").record_offset.value;
        first_id = named(*image, "first").sfs_id;
        const auto &partition = image->partitions().front();
        const auto root_id = axk::locate_partition_root_record(partition).value();
        const auto &root = *std::ranges::find(partition.records, root_id, &axk::IndexRecord::sfs_id);
        const auto &entry = *std::ranges::find(root.directory_entries, "second", &axk::DirectoryEntry::name);
        second_link = static_cast<std::uint64_t>(partition.start_sector) * 512U +
                      static_cast<std::uint64_t>(root.extents.front().cluster_offset) * 1024U +
                      entry.payload_relative_offset + 4U;
    }
    std::array<std::byte, 4> link{};
    ASSERT_TRUE(axk::ByteWriter{link}.write_be32(0U, first_id.value));
    patch(aliases, second_link, link);
    const std::array<std::byte, 72> unused{};
    patch(aliases, second_index, unused);
    const std::array<std::byte, 2> links{std::byte{}, std::byte{2}};
    patch(aliases, first_index + 0x46U, links);
    const std::array<std::byte, 4> attributes{std::byte{0xf4}, std::byte{}, std::byte{}, std::byte{}};
    patch(aliases, first_index + 0x42U, attributes);
    const auto before = digest(aliases);
    {
        const auto reader = axk::FileReader::open(aliases);
        ASSERT_TRUE(reader);
        const std::vector<axk::FilesystemImportEntry> inputs{
            {{"first"}, false, 12U, axk::FileConflict::replace},
            {{"second"}, false, 7U},
            {{"first"}, false, 9U},
        };
        const auto review = axk::inspect_sfs_file_import(*reader, axk::PartitionIndex{0}, inputs);
        ASSERT_TRUE(review) << review.error().message;
        EXPECT_EQ(review->at(0).action, axk::FilesystemImportAction::replace_file);
        EXPECT_EQ(review->at(0).existing_size_bytes, 4096U);
        EXPECT_EQ(review->at(1).action, axk::FilesystemImportAction::skip_file);
        EXPECT_EQ(review->at(1).existing_size_bytes, 4096U);
        EXPECT_EQ(review->at(2).action, axk::FilesystemImportAction::skip_file);
        EXPECT_EQ(review->at(2).existing_size_bytes, 12U);
        EXPECT_EQ(digest(aliases), before);
    }
    const auto output = folder / "replaced.hds";
    const std::vector<axk::FilesystemEdit> replacement{
        axk::PutFilesystemFile{{"first"}, data(12, std::byte{0x12}), axk::FileConflict::replace}};
    const auto result = axk::write_sfs_file_edits(aliases, output, axk::PartitionIndex{0}, replacement);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(digest(aliases), before);
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    const auto &first = named(*image, "first");
    const auto &second = named(*image, "second");
    EXPECT_NE(first.sfs_id, second.sfs_id);
    EXPECT_EQ(first.attributes, 0xf4000000U);
    EXPECT_EQ(second.attributes, 0xf4000000U);
    EXPECT_EQ(first.cluster_count, 4U);
    EXPECT_EQ(second.cluster_count, 4U);
    EXPECT_EQ(first.extents.front().cluster_offset % 4U, 0U);
    EXPECT_EQ(second.extents.front().cluster_offset % 4U, 0U);
    EXPECT_EQ(first.link_count, 1U);
    EXPECT_EQ(second.link_count, 1U);
    EXPECT_EQ(*image->read_record_data(axk::PartitionIndex{0}, first.sfs_id, 4096U),
              std::vector<std::byte>(12, std::byte{0x12}));
    EXPECT_EQ(*image->read_record_data(axk::PartitionIndex{0}, second.sfs_id, 4096U),
              std::vector<std::byte>(4096, std::byte{0x5a}));
}

TEST_F(SfsFiles, ClearsNonDirectoryTypeOnlyWhenUnlinkLeavesOneReference) {
    for (const auto low_type : {0x006c6e6bU, 0x00123456U}) {
        SCOPED_TRACE(std::format("low type {:08x}", low_type));
        const auto aliases = folder / std::format("aliases-{:08x}.hds", low_type);
        const std::vector<axk::FilesystemEdit> initial{axk::PutFilesystemFile{{"first"}, data(64)},
                                                       axk::PutFilesystemFile{{"second"}, data(0)},
                                                       axk::PutFilesystemFile{{"third"}, data(0)}};
        ASSERT_TRUE(axk::write_sfs_file_edits(source, aliases, axk::PartitionIndex{0}, initial));
        axk::SfsId first_id;
        std::uint64_t first_index{};
        std::uint32_t payload_cluster{};
        std::array<std::uint64_t, 2> discarded_indexes{};
        std::array<std::uint64_t, 2> alias_links{};
        {
            const auto image = axk::open_image(aliases);
            ASSERT_TRUE(image);
            const auto &record = named(*image, "first");
            first_id = record.sfs_id;
            first_index = record.record_offset.value;
            ASSERT_EQ(record.extents.size(), 1U);
            payload_cluster = record.extents.front().cluster_offset;
            const auto &partition = image->partitions().front();
            const auto root_id = axk::locate_partition_root_record(partition).value();
            const auto &directory = *std::ranges::find(partition.records, root_id, &axk::IndexRecord::sfs_id);
            const auto sector_bytes = image->superblock().sector_size_bytes;
            const auto cluster_bytes = partition.sectors_per_cluster * sector_bytes;
            const std::array<std::string_view, 2> names{"second", "third"};
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto &unused_record = named(*image, names[i]);
                ASSERT_TRUE(unused_record.extents.empty());
                discarded_indexes[i] = unused_record.record_offset.value;
                const auto entry = std::ranges::find(directory.directory_entries, names[i], &axk::DirectoryEntry::name);
                ASSERT_NE(entry, directory.directory_entries.end());
                alias_links[i] = static_cast<std::uint64_t>(partition.start_sector) * sector_bytes +
                                 static_cast<std::uint64_t>(directory.extents.front().cluster_offset) * cluster_bytes +
                                 entry->payload_relative_offset + 4U;
            }
        }
        // Form three names for one payload without allocating data for the discarded records.
        std::array<std::byte, 4> link{};
        ASSERT_TRUE(axk::ByteWriter{link}.write_be32(0U, first_id.value));
        const std::array<std::byte, 72> unused{};
        for (std::size_t i = 0; i < alias_links.size(); ++i) {
            ASSERT_NO_FATAL_FAILURE(patch(aliases, alias_links[i], link));
            ASSERT_NO_FATAL_FAILURE(patch(aliases, discarded_indexes[i], unused));
        }
        const std::array<std::byte, 2> links{std::byte{}, std::byte{3}};
        ASSERT_NO_FATAL_FAILURE(patch(aliases, first_index + 0x46U, links));
        constexpr std::uint32_t upper_flags = 0xde000000U;
        std::array<std::byte, 4> attributes{};
        ASSERT_TRUE(axk::ByteWriter{attributes}.write_be32(0U, upper_flags | low_type));
        ASSERT_NO_FATAL_FAILURE(patch(aliases, first_index + 0x42U, attributes));

        auto input = aliases;
        const std::array<std::string, 2> removed_names{"first", "second"};
        for (std::size_t step = 0; step < removed_names.size(); ++step) {
            const auto before = digest(input);
            const auto output = folder / std::format("unlink-{:08x}-{}.hds", low_type, step);
            const std::vector<axk::FilesystemEdit> edits{axk::RemoveFilesystemEntry{{removed_names[step]}, false}};
            const auto result = axk::write_sfs_file_edits(input, output, axk::PartitionIndex{0}, edits);
            ASSERT_TRUE(result) << result.error().message;
            EXPECT_EQ(digest(input), before);
            {
                const auto image = axk::open_image(output);
                ASSERT_TRUE(image);
                const auto &survivor = named(*image, "third");
                EXPECT_EQ(survivor.sfs_id, first_id);
                EXPECT_EQ(survivor.record_offset.value, first_index);
                EXPECT_EQ(survivor.link_count, 2U - step);
                EXPECT_EQ(survivor.attributes, upper_flags | (step == 0U ? low_type : 0U));
                ASSERT_EQ(survivor.extents.size(), 1U);
                EXPECT_EQ(survivor.extents.front().cluster_offset, payload_cluster);
                const auto payload = image->read_record_data(axk::PartitionIndex{0}, first_id, 4096U);
                ASSERT_TRUE(payload);
                EXPECT_EQ(*payload, std::vector<std::byte>(64, std::byte{0x5a}));
                if (step == 0U)
                    EXPECT_EQ(named(*image, "second").sfs_id, first_id);
                const auto &partition = image->partitions().front();
                const auto root_id = axk::locate_partition_root_record(partition).value();
                const auto &directory = *std::ranges::find(partition.records, root_id, &axk::IndexRecord::sfs_id);
                for (std::size_t removed = 0; removed <= step; ++removed)
                    EXPECT_FALSE(std::ranges::any_of(directory.directory_entries, [&](const auto &entry) {
                        return entry.name == removed_names[removed] &&
                               axk::directory_entry_state(entry.raw_link_id) == axk::DirectoryEntryState::live;
                    }));
            }
            input = output;
        }
    }
}

TEST_F(SfsFiles, RewritesLargeDirectoriesWithAlignedCapacityAndKeepsRenameInPlace) {
    const auto initial = folder / "large-directory.hds";
    const std::vector<axk::FilesystemEdit> create{axk::CreateFilesystemDirectory{{"Directory"}}};
    ASSERT_TRUE(axk::write_sfs_file_edits(source, initial, axk::PartitionIndex{0}, create));
    ASSERT_NO_FATAL_FAILURE(promote_large(initial, "Directory", 4U));
    const auto original = axk::open_image(initial).value();
    const auto reader = axk::FileReader::open(initial).value();
    const std::vector<axk::FilesystemEdit> rename{axk::RenameFilesystemEntry{{"Directory"}, "Renamed"}};
    const auto renamed = axk::detail::prepare_sfs_file_edits(reader, axk::PartitionIndex{0}, rename);
    ASSERT_TRUE(renamed) << renamed.error().message;
    const auto rename_image = axk::open_image(renamed->preview, {}).value();
    EXPECT_EQ(named(rename_image, "Renamed").extents.front().cluster_offset,
              named(original, "Directory").extents.front().cluster_offset);
    std::vector<axk::FilesystemEdit> additions;
    for (unsigned index = 0; index < 150U; ++index)
        additions.push_back(axk::PutFilesystemFile{{"Renamed", std::format("file{}", index)}, data(0)});
    const auto grown = axk::detail::prepare_sfs_file_edits(renamed->preview, axk::PartitionIndex{0}, additions);
    ASSERT_TRUE(grown) << grown.error().message;
    const auto grown_image = axk::open_image(grown->preview, {}).value();
    const auto &directory = named(grown_image, "Renamed");
    EXPECT_EQ(directory.attributes, 0xb4646972U);
    EXPECT_GT(directory.data_size, 4096U);
    EXPECT_EQ(directory.cluster_count, 8U);
    EXPECT_EQ(directory.directory_entries.size(), 152U);
    for (const auto &extent : directory.extents) {
        EXPECT_EQ(extent.cluster_offset % 4U, 0U);
        EXPECT_EQ(extent.cluster_count % 4U, 0U);
    }
    const std::vector<axk::FilesystemEdit> remove{axk::RemoveFilesystemEntry{{"Renamed"}, true}};
    const auto removed = axk::detail::prepare_sfs_file_edits(grown->preview, axk::PartitionIndex{0}, remove);
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_TRUE(
        axk::allocation_is_safe_for_mutation(axk::open_image(removed->preview, {})->partitions().front().allocation));
}

TEST_F(SfsFiles, AllocatesFragmentedWholeUnitsAndOrdinaryContinuationMetadata) {
    const auto initial = folder / "large-fragments.hds";
    std::vector<axk::FilesystemEdit> create{axk::PutFilesystemFile{{"target"}, data(0)}};
    for (unsigned index = 0; index < 14U; ++index)
        create.push_back(axk::PutFilesystemFile{{std::format("block{}", index)}, data(1)});
    ASSERT_TRUE(axk::write_sfs_file_edits(source, initial, axk::PartitionIndex{0}, create));
    ASSERT_NO_FATAL_FAILURE(promote_large(initial, "target", 4U));
    for (unsigned index = 0; index < 14U; ++index)
        ASSERT_NO_FATAL_FAILURE(promote_large(initial, std::format("block{}", index), 4U));
    const auto image = axk::open_image(initial).value();
    const auto free = image.partitions().front().allocation.free_space->free_cluster_count;
    std::vector<std::pair<std::uint32_t, std::string>> blocks;
    for (unsigned index = 0; index < 14U; ++index) {
        const auto name = std::format("block{}", index);
        blocks.emplace_back(named(image, name).extents.front().cluster_offset, name);
    }
    std::ranges::sort(blocks);
    const std::vector<axk::FilesystemEdit> fill{axk::PutFilesystemFile{{"filler"}, data((free - 4U) * 1024U)}};
    const auto reader = axk::FileReader::open(initial).value();
    const auto packed = axk::detail::prepare_sfs_file_edits(reader, axk::PartitionIndex{0}, fill);
    ASSERT_TRUE(packed) << packed.error().message;
    std::vector<axk::FilesystemEdit> edit;
    for (std::size_t index = 0; index < blocks.size(); index += 2U)
        edit.push_back(axk::RemoveFilesystemEntry{{blocks[index].second}, false});
    edit.push_back(axk::PutFilesystemFile{{"target"}, data(20U * 1024U), axk::FileConflict::replace});
    const auto changed = axk::detail::prepare_sfs_file_edits(packed->preview, axk::PartitionIndex{0}, edit);
    ASSERT_TRUE(changed) << changed.error().message;
    const auto output = axk::open_image(changed->preview, {}).value();
    const auto &record = named(output, "target");
    EXPECT_GT(record.extents.size(), 4U);
    EXPECT_EQ(record.cluster_count, 20U);
    ASSERT_EQ(record.continuation_clusters.size(), 1U);
    for (const auto &extent : record.extents) {
        EXPECT_EQ(extent.cluster_offset % 4U, 0U);
        EXPECT_EQ(extent.cluster_count % 4U, 0U);
    }
    EXPECT_EQ(*output.read_record_data(axk::PartitionIndex{0}, record.sfs_id, 32768U),
              std::vector<std::byte>(20U * 1024U, std::byte{0x5a}));
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(output.partitions().front().allocation));
}

TEST_F(SfsFiles, RejectsUnusableLargeUnitsWithoutBlockingReadsRenameOrEmptyReplacement) {
    for (const auto unit : {0U, 65536U, std::numeric_limits<std::uint32_t>::max()}) {
        SCOPED_TRACE(unit);
        const auto initial = folder / std::format("unit-{}.hds", unit);
        const std::vector<axk::FilesystemEdit> create{axk::PutFilesystemFile{{"target"}, data(0)}};
        ASSERT_TRUE(axk::write_sfs_file_edits(source, initial, axk::PartitionIndex{0}, create));
        ASSERT_NO_FATAL_FAILURE(promote_large(initial, "target", 4U));
        const auto image = axk::open_image(initial).value();
        const auto base = static_cast<std::uint64_t>(image.partitions().front().start_sector) * 512U;
        std::array<std::byte, 4> word{};
        ASSERT_TRUE(axk::ByteWriter{word}.write_be32(0U, unit));
        patch(initial, base + 0x84U, word);
        patch(initial, base + 1024U + 0x84U, word);
        const auto before = digest(initial);
        ASSERT_TRUE(axk::open_image(initial));
        const std::vector<axk::FilesystemEdit> replace{
            axk::CreateFilesystemDirectory{{"Not committed"}},
            axk::PutFilesystemFile{{"target"}, data(1U), axk::FileConflict::replace}};
        const auto output = folder / std::format("invalid-{}.hds", unit);
        EXPECT_FALSE(axk::write_sfs_file_edits(initial, output, axk::PartitionIndex{0}, replace));
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(digest(initial), before);
        const std::vector<axk::FilesystemEdit> harmless{
            axk::PutFilesystemFile{{"target"}, data(0U), axk::FileConflict::replace},
            axk::RenameFilesystemEntry{{"target"}, "renamed"}, axk::RemoveFilesystemEntry{{"renamed"}, false}};
        const auto reader = axk::FileReader::open(initial).value();
        const auto prepared = axk::detail::prepare_sfs_file_edits(reader, axk::PartitionIndex{0}, harmless);
        ASSERT_TRUE(prepared) << prepared.error().message;
        EXPECT_EQ(digest(initial), before);
    }
}

TEST_F(SfsFiles, RejectsPartialUnitSpaceWithoutPublishingEarlierBatchEdits) {
    const auto initial = folder / "partial-units.hds";
    std::vector<axk::FilesystemEdit> create{axk::PutFilesystemFile{{"target"}, data(0)}};
    for (unsigned index = 0; index < 14U; ++index)
        create.push_back(axk::PutFilesystemFile{{std::format("block{}", index)}, data(1)});
    ASSERT_TRUE(axk::write_sfs_file_edits(source, initial, axk::PartitionIndex{0}, create));
    ASSERT_NO_FATAL_FAILURE(promote_large(initial, "target", 4U));
    const auto free = axk::open_image(initial)->partitions().front().allocation.free_space->free_cluster_count;
    const std::vector<axk::FilesystemEdit> fill{axk::PutFilesystemFile{{"filler"}, data((free - 2U) * 1024U)}};
    const auto packed = folder / "partial-packed.hds";
    ASSERT_TRUE(axk::write_sfs_file_edits(initial, packed, axk::PartitionIndex{0}, fill));
    const auto before = digest(packed);
    const auto reader = axk::FileReader::open(packed).value();
    std::vector<axk::FilesystemEdit> edit;
    for (unsigned index = 0; index < 14U; index += 2U)
        edit.push_back(axk::RemoveFilesystemEntry{{std::format("block{}", index)}, false});
    const auto freed = axk::detail::prepare_sfs_file_edits(reader, axk::PartitionIndex{0}, edit);
    ASSERT_TRUE(freed) << freed.error().message;
    EXPECT_GE(axk::open_image(freed->preview, {})->partitions().front().allocation.free_space->free_cluster_count, 4U);
    edit.push_back(axk::PutFilesystemFile{{"target"}, data(1), axk::FileConflict::replace});
    const auto output = folder / "no-whole-unit.hds";
    const auto rejected = axk::write_sfs_file_edits(packed, output, axk::PartitionIndex{0}, edit);
    ASSERT_FALSE(rejected);
    EXPECT_NE(rejected.error().message.find("insufficient free clusters"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(digest(packed), before);
}

class GeneratedFile final : public axk::RandomAccessReader {
  public:
    std::uint64_t length{2U * 1024U * 1024U + 1U};
    mutable std::size_t largest_read{};
    mutable unsigned passes{};
    bool fail_on_second_pass{};
    bool change_on_second_pass{};
    std::uint64_t size() const noexcept override { return length; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> output) const override {
        largest_read = std::max(largest_read, output.size());
        if (offset == 0)
            ++passes;
        if (fail_on_second_pass && passes > 1)
            return std::unexpected{
                axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "injected read failure")};
        std::ranges::fill(output, change_on_second_pass && passes > 1 ? std::byte{0x17} : std::byte{0x5a});
        return {};
    }
};

TEST_F(SfsFiles, RejectsRoundedClusterCountOverflowBeforeReadingPayload) {
    const auto initial = folder / "overflow.hds";
    const std::vector<axk::FilesystemEdit> create{axk::PutFilesystemFile{{"large"}, data(0)}};
    ASSERT_TRUE(axk::write_sfs_file_edits(source, initial, axk::PartitionIndex{0}, create));
    ASSERT_NO_FATAL_FAILURE(promote_large(initial, "large", 200U));
    const auto payload = std::make_shared<GeneratedFile>();
    payload->length = 65500U * 1024U;
    const auto reader = axk::FileReader::open(initial).value();
    const std::vector<axk::FilesystemEdit> replace{
        axk::PutFilesystemFile{{"large"}, payload, axk::FileConflict::replace}};
    const auto rejected = axk::detail::prepare_sfs_file_edits(reader, axk::PartitionIndex{0}, replace);
    ASSERT_FALSE(rejected);
    EXPECT_NE(rejected.error().message.find("cluster-count"), std::string::npos);
    EXPECT_EQ(payload->passes, 0U);
}

TEST_F(SfsFiles, StreamsLargeFilesAndDiscardsFailedOrStaleInputPublications) {
    const auto before = digest(source);
    for (unsigned mode = 0; mode < 3; ++mode) {
        auto input = std::make_shared<GeneratedFile>();
        input->fail_on_second_pass = mode == 1;
        input->change_on_second_pass = mode == 2;
        const std::vector<axk::FilesystemEdit> edits{axk::PutFilesystemFile{{"large"}, input}};
        const auto output = folder / std::format("stream{}.hds", mode);
        const auto result = axk::write_sfs_file_edits(source, output, axk::PartitionIndex{0}, edits);
        EXPECT_EQ(result.has_value(), mode == 0);
        if (mode == 2 && !result) {
            EXPECT_EQ(result.error().code, axk::ErrorCode::transaction_stale);
        }
        EXPECT_EQ(std::filesystem::exists(output), mode == 0);
        EXPECT_LE(input->largest_read, 1024U * 1024U);
        EXPECT_EQ(digest(source), before);
    }
    std::vector<std::string> names;
    for (const auto &entry : std::filesystem::directory_iterator{folder}) {
        if (entry.path().filename() == ".axklib-publication") {
            EXPECT_TRUE(std::filesystem::is_empty(entry.path()));
            continue;
        }
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    EXPECT_EQ(names, (std::vector<std::string>{"source.hds", "stream0.hds"}));
}

TEST_F(SfsFiles, RefusesExhaustionAndExistingDestinationsWithoutPublishingPartialEdits) {
    const auto before = digest(source);
    auto huge = std::make_shared<GeneratedFile>();
    huge->length = 16U * 1024U * 1024U;
    const std::vector<axk::FilesystemEdit> edits{axk::CreateFilesystemDirectory{{"Fits"}},
                                                 axk::PutFilesystemFile{{"too large"}, huge}};
    EXPECT_FALSE(axk::write_sfs_file_edits(source, folder / "full.hds", axk::PartitionIndex{0}, edits));
    EXPECT_FALSE(std::filesystem::exists(folder / "full.hds"));
    const std::vector<axk::FilesystemEdit> empty{axk::PutFilesystemFile{{"empty"}, data(0)}};
    EXPECT_FALSE(axk::write_sfs_file_edits(source, source, axk::PartitionIndex{0}, empty));
    EXPECT_EQ(digest(source), before);
}

TEST_F(SfsFiles, DoesNotInterpretDirectoryLookingRawFileContentsAsFilesystemLinks) {
    std::vector<std::byte> directory_bytes;
    {
        const auto original = axk::open_image(source);
        ASSERT_TRUE(original);
        const auto root = axk::locate_partition_root_record(original->partitions().front()).value();
        directory_bytes = *original->read_record_data(axk::PartitionIndex{0}, root, 65536U);
    }
    const std::vector<axk::FilesystemEdit> edits{
        axk::PutFilesystemFile{{"directory.bin"}, std::make_shared<axk::MemoryReader>(directory_bytes)}};
    const auto output = folder / "directory-data.hds";
    const auto result = axk::write_sfs_file_edits(source, output, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(result) << result.error().message;
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    const auto &record = named(*image, "directory.bin");
    EXPECT_EQ(record.payload_kind, axk::PayloadKind::unknown);
    EXPECT_TRUE(record.directory_entries.empty());
    EXPECT_EQ(*image->read_record_data(axk::PartitionIndex{0}, record.sfs_id, 65536U), directory_bytes);
}

TEST_F(SfsFiles, CancelsDuringCopyWithoutPublishingOrChangingSource) {
    class CancelCopy final : public axk::ProgressSink {
      public:
        axk::CancellationSource cancellation;
        void report(const axk::Progress &) noexcept override { cancellation.cancel(); }
    } progress;
    const auto before = digest(source);
    const std::vector<axk::FilesystemEdit> edits{axk::PutFilesystemFile{{"new"}, data(8192)}};
    const auto output = folder / "cancelled.hds";
    const auto result = axk::write_sfs_file_edits(source, output, axk::PartitionIndex{0}, edits,
                                                  progress.cancellation.token(), &progress);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(digest(source), before);
}

TEST_F(SfsFiles, PreservesSamplerAuthoredObjectPayloadsAndUntouchedIndexBytes) {
    const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} /
                         "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds";
    const auto before = digest(fixture);
    const std::vector<axk::FilesystemEdit> edits{axk::CreateFilesystemDirectory{{"Raw files"}},
                                                 axk::PutFilesystemFile{{"Raw files", "plain.bin"}, data(10001)}};
    const auto output = folder / "sampler-copy.hds";
    const auto result = axk::write_sfs_file_edits(fixture, output, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(result) << result.error().message;
    const auto original = axk::open_image(fixture);
    const auto changed = axk::open_image(output);
    ASSERT_TRUE(original);
    ASSERT_TRUE(changed);
    const auto original_reader = axk::FileReader::open(fixture).value();
    const auto changed_reader = axk::FileReader::open(output).value();
    const auto root = axk::locate_partition_root_record(original->partitions().front()).value();
    for (const auto &record : original->partitions().front().records) {
        if (record.sfs_id == root)
            continue;
        const auto &current =
            *std::ranges::find(changed->partitions().front().records, record.sfs_id, &axk::IndexRecord::sfs_id);
        EXPECT_EQ(*original->read_record_data(axk::PartitionIndex{0}, record.sfs_id, 1048576U),
                  *changed->read_record_data(axk::PartitionIndex{0}, current.sfs_id, 1048576U));
        std::array<std::byte, 72> original_index{};
        std::array<std::byte, 72> changed_index{};
        ASSERT_TRUE(original_reader->read_exact_at(record.record_offset.value, original_index));
        ASSERT_TRUE(changed_reader->read_exact_at(current.record_offset.value, changed_index));
        EXPECT_EQ(original_index, changed_index);
    }
    EXPECT_EQ(digest(fixture), before);
}

TEST_F(SfsFiles, ReplacesSamplerAuthoredLargePayloadsWithoutChangingOtherRecords) {
    const auto fixtures = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored";
    std::vector<std::filesystem::path> sources{fixtures / "HD00_512_single_sbnk_authored.hds",
                                               fixtures / "HD00_512_multi_sbnk_authored.hds"};
    if (const auto external = external_sfs_image()) {
        sources.emplace_back(*external);
        RecordProperty("external_image", *external);
    }
    unsigned image_number{};
    for (const auto &input : sources) {
        SCOPED_TRACE(input.string());
        const auto before = digest(input);
        const auto original = axk::open_image(input).value();
        const auto &partition = original.partitions().front();
        const auto root_id = axk::locate_partition_root_record(partition).value();
        std::vector<axk::FilesystemEdit> replacements;
        for (const auto &record : partition.records) {
            if ((record.attributes & 0x20000000U) == 0U || record.data_size == 0U)
                continue;
            ASSERT_NE(record.payload_kind, axk::PayloadKind::directory);
            axk::FilesystemPath path;
            auto owner = record.sfs_id;
            for (std::size_t depth = 0; owner != root_id && depth < partition.records.size(); ++depth) {
                bool found = false;
                for (const auto &directory : partition.records) {
                    const auto entry = std::ranges::find_if(directory.directory_entries, [&](const auto &candidate) {
                        return candidate.state == axk::DirectoryEntryState::live && candidate.name != "." &&
                               candidate.name != ".." && candidate.raw_link_id.value == owner.value;
                    });
                    if (entry == directory.directory_entries.end())
                        continue;
                    path.insert(path.begin(), entry->name);
                    owner = directory.sfs_id;
                    found = true;
                    break;
                }
                ASSERT_TRUE(found);
            }
            ASSERT_EQ(owner, root_id);
            const auto payload = original.read_record_data(partition.index, record.sfs_id, 16U * 1024U * 1024U).value();
            replacements.push_back(
                axk::PutFilesystemFile{path, std::make_shared<axk::MemoryReader>(payload), axk::FileConflict::replace});
        }
        ASSERT_FALSE(replacements.empty());
        const auto output = folder / std::format("large-source-{}.hds", image_number++);
        const auto written = axk::write_sfs_file_edits(input, output, partition.index, replacements);
        ASSERT_TRUE(written) << written.error().message;
        EXPECT_EQ(digest(input), before);
        const auto changed = axk::open_image(output).value();
        const auto input_reader = axk::FileReader::open(input).value();
        const auto output_reader = axk::FileReader::open(output).value();
        EXPECT_TRUE(axk::allocation_is_safe_for_mutation(changed.partitions().front().allocation));
        for (const auto &record : partition.records) {
            const auto &current =
                *std::ranges::find(changed.partitions().front().records, record.sfs_id, &axk::IndexRecord::sfs_id);
            EXPECT_EQ(current.attributes, record.attributes);
            EXPECT_EQ(*original.read_record_data(partition.index, record.sfs_id, 16U * 1024U * 1024U),
                      *changed.read_record_data(partition.index, current.sfs_id, 16U * 1024U * 1024U));
            if ((record.attributes & 0x20000000U) != 0U) {
                for (const auto &extent : current.extents) {
                    EXPECT_EQ(extent.cluster_offset % partition.large_allocation_unit_clusters, 0U);
                    EXPECT_EQ(extent.cluster_count % partition.large_allocation_unit_clusters, 0U);
                }
            } else {
                std::array<std::byte, 72> old_index{}, new_index{};
                ASSERT_TRUE(input_reader->read_exact_at(record.record_offset.value, old_index));
                ASSERT_TRUE(output_reader->read_exact_at(current.record_offset.value, new_index));
                EXPECT_EQ(old_index, new_index);
            }
        }
    }
}

TEST_F(SfsFiles, KeepsEveryByteOutsideTheSelectedPartitionUnchanged) {
    const auto multiple = folder / "multiple.hds";
    const axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {{"First", {}}, {"Second", {}}}};
    ASSERT_TRUE(axk::write_hds_image(manifest, multiple));
    const std::vector<axk::FilesystemEdit> edits{axk::PutFilesystemFile{{"new"}, data(10001)}};
    const auto output = folder / "second-edited.hds";
    const auto result = axk::write_sfs_file_edits(multiple, output, axk::PartitionIndex{1}, edits);
    ASSERT_TRUE(result) << result.error().message;
    const auto original = axk::open_image(multiple);
    ASSERT_TRUE(original);
    const auto &selected = original->partitions().at(1);
    const auto begin = static_cast<std::uint64_t>(selected.start_sector) * 512U;
    const auto end = begin + static_cast<std::uint64_t>(selected.sector_count) * 512U;
    const auto input = axk::FileReader::open(multiple).value();
    const auto edited = axk::FileReader::open(output).value();
    const auto hash_range = [](const axk::RandomAccessReader &reader, std::uint64_t start, std::uint64_t finish) {
        std::vector<std::byte> buffer(65536U);
        axk::package_internal::Sha256State hash;
        while (start < finish) {
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), finish - start));
            const auto chunk = std::span{buffer}.first(size);
            EXPECT_TRUE(reader.read_exact_at(start, chunk));
            hash.update(chunk);
            start += size;
        }
        return hash.finish();
    };
    EXPECT_EQ(input->size(), edited->size());
    EXPECT_EQ(hash_range(*input, 0U, begin), hash_range(*edited, 0U, begin));
    EXPECT_EQ(hash_range(*input, end, input->size()), hash_range(*edited, end, edited->size()));
}
} // namespace
