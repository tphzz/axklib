#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
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

namespace {
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
};

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
    ASSERT_EQ(directory.directory_entries.size(), 4U);
    for (const auto &[name, size] :
         std::vector<std::pair<std::string, std::size_t>>{{"empty.bin", 0}, {"payload.bin", 8193}}) {
        const auto file = std::ranges::find(directory.directory_entries, name, &axk::DirectoryEntry::name);
        ASSERT_NE(file, directory.directory_entries.end());
        const auto id = axk::SfsId{file->raw_link_id.value};
        const auto &record = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
        EXPECT_EQ(record.data_size, size);
        EXPECT_EQ(record.link_count, 1U);
        const auto bytes = image->read_record_data(partition.index, id, 16384U);
        ASSERT_TRUE(bytes);
        EXPECT_EQ(*bytes, std::vector<std::byte>(size, std::byte{0x5a}));
        if (size == 0) {
            EXPECT_TRUE(record.extents.empty());
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
    EXPECT_EQ(prepared->source_snapshot_id, axk::package_internal::hex_digest(before));
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
    const std::array<std::byte, 4> attributes{std::byte{0xd4}, std::byte{}, std::byte{}, std::byte{}};
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
    EXPECT_EQ(first.attributes, 0xd4000000U);
    EXPECT_EQ(second.attributes, 0xd4000000U);
    EXPECT_EQ(first.link_count, 1U);
    EXPECT_EQ(second.link_count, 1U);
    EXPECT_EQ(*image->read_record_data(axk::PartitionIndex{0}, first.sfs_id, 4096U),
              std::vector<std::byte>(12, std::byte{0x12}));
    EXPECT_EQ(*image->read_record_data(axk::PartitionIndex{0}, second.sfs_id, 4096U),
              std::vector<std::byte>(4096, std::byte{0x5a}));
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
