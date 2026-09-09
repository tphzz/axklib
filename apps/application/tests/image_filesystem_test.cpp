#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "../../../library/tests/media_test_fixtures.hpp"
#include "../src/image_filesystem_internal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/bytes.hpp"
#include "axklib/lookups.hpp"
#include "axklib/writer.hpp"

namespace {
class ImageFilesystemTest : public testing::Test {
  protected:
    void SetUp() override {
        const auto root = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored";
        auto sandbox = axk::app::Sandbox::create({{"fixtures", "Fixtures", root, false}});
        ASSERT_TRUE(sandbox) << sandbox.error().message;
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        sessions_ = std::make_unique<axk::app::ImageSessionManager>(*sandbox_);
        const auto opened = sessions_->open({"fixtures", "HD00_512_single_sbnk_authored.hds"}, "owner");
        ASSERT_TRUE(opened) << opened.error().message;
        id_ = opened->image_id;
    }
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    std::unique_ptr<axk::app::ImageSessionManager> sessions_;
    std::string id_;
};

TEST_F(ImageFilesystemTest, ReadsPhysicalEntriesAndMapsObjectsWithoutDuplicatingRelationships) {
    const auto roots = sessions_->filesystem(id_, "owner", 1U);
    ASSERT_TRUE(roots) << roots.error().message;
    EXPECT_TRUE(roots->available);
    EXPECT_EQ(roots->filesystem_name, "Yamaha SFS");
    EXPECT_EQ(roots->device_view, "a-series");
    ASSERT_EQ(roots->root_capabilities.size(), roots->items.size());
    EXPECT_FALSE(roots->root_capabilities.front().create_directory);
    EXPECT_FALSE(roots->root_capabilities.front().put_file);
    EXPECT_FALSE(roots->root_capabilities.front().delete_entry);
    ASSERT_FALSE(roots->items.empty());
    const auto root_id = roots->items.front().id;
    EXPECT_EQ(roots->items.front().kind, "partition");
    const auto matches = sessions_->filesystem(id_, "owner", 1U, {.root_id = root_id, .query = "smp"});
    ASSERT_TRUE(matches) << matches.error().message;
    for (const auto &entry : matches->items) {
        EXPECT_EQ(entry.root_id, root_id);
        EXPECT_FALSE(entry.ancestor_ids.empty());
    }
    const auto objects = sessions_->objects(id_, "owner", 100U);
    ASSERT_TRUE(objects);
    ASSERT_FALSE(objects->items.empty());
    for (const auto &object : objects->items) {
        const auto mapped = sessions_->filesystem(id_, "owner", 1U, {.object_id = object.id});
        ASSERT_TRUE(mapped);
        ASSERT_EQ(mapped->items.size(), 1U) << object.name;
        EXPECT_EQ(mapped->items.front().object_id, object.id);
        EXPECT_EQ(mapped->items.front().kind, "file");
        EXPECT_EQ(mapped->items.front().child_count, 0U);
        EXPECT_EQ(mapped->items.front().name, object.entry_name);
    }
}

TEST_F(ImageFilesystemTest, EnforcesOwnershipRevisionScopeAndPageBounds) {
    EXPECT_FALSE(sessions_->filesystem(id_, "another-owner", 1U));
    EXPECT_FALSE(sessions_->filesystem(id_, "owner", 2U));
    EXPECT_FALSE(sessions_->filesystem(id_, "owner", 1U, {.parent_id = "missing"}));
    EXPECT_FALSE(sessions_->filesystem(id_, "owner", 1U, {.query = "x"}));
    EXPECT_FALSE(sessions_->filesystem(id_, "owner", 1U, {.limit = 0U}));
    const auto roots = sessions_->filesystem(id_, "owner", 1U, {.limit = 1U});
    ASSERT_TRUE(roots);
    ASSERT_EQ(roots->items.size(), 1U);
    const auto next = sessions_->filesystem(id_, "owner", 1U, {.offset = 1U, .limit = 1U});
    ASSERT_TRUE(next);
    EXPECT_EQ(next->total_count, roots->total_count);
}

TEST_F(ImageFilesystemTest, ReservedRootEntriesDoNotReportDanglingFileWarnings) {
    const auto roots = sessions_->filesystem(id_, "owner", 1U);
    ASSERT_TRUE(roots);
    const auto entries = sessions_->filesystem(id_, "owner", 1U, {.parent_id = roots->items.front().id});
    ASSERT_TRUE(entries);
    const auto log = std::ranges::find(entries->items, "sfserrlog", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(log, entries->items.end());
    EXPECT_TRUE(log->issue.empty());
    EXPECT_TRUE(log->filesystem_metadata);
    EXPECT_EQ(log->size_bytes, 0U);
    EXPECT_EQ(log->raw_attributes, "SFS 0x94000000");
    EXPECT_NE(log->storage.find("link count 1"), std::string::npos);
}

TEST_F(ImageFilesystemTest, ResolvesFilesystemEditsFromEntryIdentityAndStoredAncestry) {
    const auto roots = sessions_->filesystem(id_, "owner", 1U);
    ASSERT_TRUE(roots);
    const auto entries = sessions_->filesystem(id_, "owner", 1U, {.parent_id = roots->items.front().id});
    ASSERT_TRUE(entries);
    const auto directory = std::ranges::find(entries->items, "directory", &axk::app::ImageFilesystemEntry::kind);
    ASSERT_NE(directory, entries->items.end());
    const std::vector<axk::app::ImageFilesystemEdit> requests{
        axk::app::CreateImageFilesystemDirectory{directory->id, {"Documents"}},
        axk::app::PutImageFilesystemFile{
            directory->id, {"Documents", "empty.bin"}, std::make_shared<axk::MemoryReader>(std::vector<std::byte>{})}};
    const auto resolved = sessions_->resolve_filesystem_edits(id_, "owner", 1U, requests);
    ASSERT_TRUE(resolved) << resolved.error().message;
    EXPECT_EQ(resolved->partition, axk::PartitionIndex{0});
    ASSERT_EQ(resolved->edits.size(), 2U);
    EXPECT_EQ(std::get<axk::CreateFilesystemDirectory>(resolved->edits[0]).path,
              (axk::FilesystemPath{directory->name, "Documents"}));
    const auto &file = std::get<axk::PutFilesystemFile>(resolved->edits[1]);
    EXPECT_EQ(file.path, (axk::FilesystemPath{directory->name, "Documents", "empty.bin"}));
    EXPECT_EQ(file.conflict, axk::FileConflict::skip);
    EXPECT_EQ(file.contents->size(), 0U);
    EXPECT_FALSE(sessions_->resolve_filesystem_edits(id_, "another-owner", 1U, requests));
    const auto stale = sessions_->resolve_filesystem_edits(id_, "owner", 2U, requests);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "image_revision_stale");
}

TEST_F(ImageFilesystemTest, RejectsInvalidFilesystemTargetsBeforeMutation) {
    const auto roots = sessions_->filesystem(id_, "owner", 1U);
    ASSERT_TRUE(roots);
    const auto entries = sessions_->filesystem(id_, "owner", 1U, {.parent_id = roots->items.front().id});
    ASSERT_TRUE(entries);
    const auto metadata = std::ranges::find(entries->items, "sfserrlog", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(metadata, entries->items.end());
    const std::vector<axk::app::ImageFilesystemEdit> rejected{
        axk::app::CreateImageFilesystemDirectory{"missing", {"New"}},
        axk::app::CreateImageFilesystemDirectory{metadata->id, {"New"}},
        axk::app::RemoveImageFilesystemEntry{roots->items.front().id, true},
        axk::app::RemoveImageFilesystemEntry{metadata->id, true},
        axk::app::CreateImageFilesystemDirectory{roots->items.front().id, {"..", "escape"}},
        axk::app::CreateImageFilesystemDirectory{roots->items.front().id, {"nested/name"}},
        axk::app::CreateImageFilesystemDirectory{roots->items.front().id, {}}};
    for (const auto &request : rejected) {
        const std::vector<axk::app::ImageFilesystemEdit> batch{request};
        EXPECT_FALSE(sessions_->resolve_filesystem_edits(id_, "owner", 1U, batch));
    }
    EXPECT_FALSE(sessions_->resolve_filesystem_edits(id_, "owner", 1U, {}));
}
} // namespace

TEST(ImageFilesystemIndex, PreservesFatNamesAndNestedDirectoriesWithoutDeviceInterpretation) {
    auto bytes = nested_fat_fixture();
    std::fill_n(bytes.begin() + 5U * 512U, 16U, std::byte{0x31});
    const auto media = axk::open_media(std::make_shared<axk::MemoryReader>(std::move(bytes)), "unknown.img");
    ASSERT_TRUE(media) << media.error().message;
    const auto index = axk::app::detail::build_image_filesystem(*media, {}, {}, {}, {});
    ASSERT_TRUE(index) << index.error().message;
    EXPECT_TRUE(index->available);
    EXPECT_FALSE(index->device_view);
    ASSERT_EQ(index->entries.size(), 3U);
    const auto &file = index->entries.back();
    EXPECT_EQ(file.name, "SMPTEST.004");
    EXPECT_EQ(file.path, "/OBJECTS/SMPTEST.004");
    EXPECT_EQ(file.parent_id, index->entries[1].id);
    EXPECT_EQ(file.ancestor_ids.size(), 2U);
    EXPECT_FALSE(file.object_id);
    EXPECT_EQ(file.raw_attributes, "FAT 0x20");
    EXPECT_EQ(file.attributes, (std::vector<std::string>{"Archive"}));
    EXPECT_EQ(index->entries[1].raw_attributes, "FAT 0x10");
    ASSERT_EQ(index->root_capabilities.size(), 1U);
    EXPECT_EQ(index->root_capabilities.front().root_id, index->entries.front().id);
    EXPECT_FALSE(index->root_capabilities.front().create_directory);
    EXPECT_FALSE(index->root_capabilities.front().put_file);
    EXPECT_FALSE(index->root_capabilities.front().delete_entry);
    const auto locator = index->files.find(file.id);
    ASSERT_NE(locator, index->files.end());
    const auto payload = axk::app::detail::read_filesystem_range(*media, locator->second, 0U, 16U, {});
    ASSERT_TRUE(payload);
    EXPECT_EQ(*payload, std::vector<std::byte>(16U, std::byte{0x31}));
    EXPECT_FALSE(axk::app::detail::read_filesystem_range(*media, locator->second, 0U, 1024U * 1024U + 1U, {}));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::app::detail::read_filesystem_range(*media, locator->second, 0U, 16U, cancellation.token()));
}

TEST(ImageFilesystemIndex, EmptyAuthoredSfsRetainsDeviceAuthoringDespiteSupportFiles) {
    struct TemporaryImage {
        std::filesystem::path path;
        ~TemporaryImage() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };
    for (const bool with_volume : {false, true}) {
        TemporaryImage temporary{
            std::filesystem::temp_directory_path() /
            std::format("axk-filesystem-{}.hds", std::chrono::steady_clock::now().time_since_epoch().count())};
        axk::HdsBuildManifest manifest{"1.0", 1'048'576U, {{"Blank", {}}}};
        if (with_volume)
            manifest.partitions.front().volumes.push_back({"Empty", {}, {}, {}, {}});
        const auto written = axk::write_hds_image(manifest, temporary.path);
        ASSERT_TRUE(written) << written.error().message;
        const auto media = axk::open_media(temporary.path);
        ASSERT_TRUE(media) << media.error().message;
        const auto index = axk::app::detail::build_image_filesystem(*media, {}, {}, {}, {});
        ASSERT_TRUE(index) << index.error().message;
        EXPECT_TRUE(index->available);
        EXPECT_EQ(index->device_view, "a-series");
        EXPECT_EQ(index->entries.front().raw_attributes, "SFS 0x94646972");
        EXPECT_TRUE(index->entries.front().attributes.empty());
    }
}

TEST(ImageFilesystemIndex, ListsIsoSupportFilesAsPhysicalEntriesRatherThanProjectedVolumes) {
    auto bytes = iso_fixture();
    const auto stored = std::span{bytes}.subspan(21U * 2048U, smpl_object().size());
    const std::vector<std::byte> expected{stored.begin(), stored.end()};
    const auto media = axk::open_media(std::make_shared<axk::MemoryReader>(std::move(bytes)), "stored.iso");
    ASSERT_TRUE(media) << media.error().message;
    const auto index = axk::app::detail::build_image_filesystem(*media, {}, {}, {}, {});
    ASSERT_TRUE(index) << index.error().message;
    EXPECT_EQ(index->filesystem_name, "ISO9660");
    const auto file = std::ranges::find(index->entries, "/GROUP/F001/F000", &axk::app::ImageFilesystemEntry::path);
    ASSERT_NE(file, index->entries.end());
    EXPECT_EQ(file->size_bytes, smpl_object().size());
    EXPECT_EQ(file->ancestor_ids.size(), 3U);
    EXPECT_NE(std::ranges::find(index->entries, "/GROUP/F002", &axk::app::ImageFilesystemEntry::path),
              index->entries.end());
    EXPECT_EQ(index->entries.front().name, "TESTVOL");
    const auto locator = index->files.find(file->id);
    ASSERT_NE(locator, index->files.end());
    const auto payload = axk::app::detail::read_filesystem_range(*media, locator->second, 0U, smpl_object().size(), {});
    ASSERT_TRUE(payload);
    EXPECT_EQ(*payload, expected);
}

TEST(ImageFilesystemIndex, DirectoryAliasesHaveDistinctDescendantIdentities) {
    const auto source = std::filesystem::path{AXK_SOURCE_ROOT} /
                        "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds";
    const auto image = axk::open_image(source);
    ASSERT_TRUE(image);
    const auto &partition = image->partitions().front();
    const auto root_id = axk::locate_partition_root_record(partition);
    ASSERT_TRUE(root_id);
    const auto root = std::ranges::find(partition.records, *root_id, &axk::IndexRecord::sfs_id);
    ASSERT_NE(root, partition.records.end());
    const auto volume = std::ranges::find_if(root->directory_entries, [&](const auto &entry) {
        if (!entry.target_link_id || entry.name == "." || entry.name == "..")
            return false;
        const auto record =
            std::ranges::find(partition.records, axk::SfsId{entry.target_link_id->value}, &axk::IndexRecord::sfs_id);
        return record != partition.records.end() && record->payload_kind == axk::PayloadKind::directory;
    });
    ASSERT_NE(volume, root->directory_entries.end());
    const auto support = std::ranges::find(root->directory_entries, "sfserram", &axk::DirectoryEntry::name);
    ASSERT_NE(support, root->directory_entries.end());
    const auto file = axk::FileReader::open(source);
    ASSERT_TRUE(file);
    std::vector<std::byte> bytes(static_cast<std::size_t>((*file)->size()));
    ASSERT_TRUE((*file)->read_exact_at(0U, bytes));
    const auto root_offset = static_cast<std::size_t>(partition.start_sector) * image->superblock().sector_size_bytes +
                             static_cast<std::size_t>(root->extents.front().cluster_offset) *
                                 partition.sectors_per_cluster * image->superblock().sector_size_bytes;
    auto alias = std::span{bytes}.subspan(root_offset + support->payload_relative_offset, 32U);
    std::ranges::copy(std::span{bytes}.subspan(root_offset + volume->payload_relative_offset, 32U), alias.begin());
    std::ranges::fill(alias.subspan(8U), std::byte{});
    ASSERT_TRUE(axk::ByteWriter{alias}.write_be16(2U, 6U));
    const std::string name{"ALIAS"};
    for (std::size_t i = 0; i < name.size(); ++i)
        alias[8U + i] = static_cast<std::byte>(name[i]);
    const auto media = axk::open_media(std::make_shared<axk::MemoryReader>(std::move(bytes)), "alias.hds");
    ASSERT_TRUE(media) << media.error().message;
    const auto index = axk::app::detail::build_image_filesystem(*media, {}, {}, {}, {});
    ASSERT_TRUE(index) << index.error().message;
    std::set<std::string> identities;
    std::size_t alias_children{};
    for (const auto &entry : index->entries) {
        EXPECT_TRUE(identities.insert(entry.id).second) << entry.path;
        if (entry.path.starts_with("/ALIAS/"))
            ++alias_children;
    }
    EXPECT_GT(alias_children, 0U);
}
