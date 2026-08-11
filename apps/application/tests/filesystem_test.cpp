#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <process.h>
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "axklib/application/filesystem.hpp"

namespace {

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class ThreadStopGuard {
  public:
    ThreadStopGuard(std::atomic_bool &stop, std::thread &thread) : stop_{stop}, thread_{thread} {}
    ThreadStopGuard(const ThreadStopGuard &) = delete;
    ThreadStopGuard &operator=(const ThreadStopGuard &) = delete;
    ~ThreadStopGuard() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable())
            thread_.join();
    }

  private:
    std::atomic_bool &stop_;
    std::thread &thread_;
};

class SandboxTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-sandbox-test";
        outside_ = std::filesystem::temp_directory_path() / "axklib-sandbox-outside-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::remove_all(outside_, error);
        std::filesystem::create_directories(root_ / "images");
        std::filesystem::create_directories(root_ / "images" / "folder");
        std::filesystem::create_directories(outside_);
        std::ofstream(root_ / "images" / "disk.hds") << "image";
        std::ofstream(root_ / "images" / "alpha.hds") << "a";
        std::ofstream(root_ / "images" / "zulu.hds") << "z";
        std::ofstream(outside_ / "secret.hds") << "secret";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::remove_all(outside_, error);
    }

    [[nodiscard]] axk::app::Sandbox sandbox() const {
        auto result = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        EXPECT_TRUE(result) << result.error().message;
        return std::move(*result);
    }

    std::filesystem::path root_;
    std::filesystem::path outside_;
};

TEST_F(SandboxTest, DiscoversRootsAndListsBoundedRelativeEntries) {
    const auto value = sandbox();
    const auto roots = value.roots();
    ASSERT_EQ(roots.size(), 1U);
    EXPECT_EQ(roots.front().id, "workspace");
    EXPECT_EQ(roots.front().display_name, "Workspace");
    EXPECT_TRUE(roots.front().writable);

    const auto listing = value.list_directory({"workspace", "images"}, 4U);
    ASSERT_TRUE(listing) << listing.error().message;
    ASSERT_EQ(listing->entries.size(), 4U);
    EXPECT_EQ(listing->entries[0].name, "folder");
    EXPECT_EQ(listing->entries[1].name, "alpha.hds");
    EXPECT_EQ(listing->entries[2].name, "disk.hds");
    EXPECT_EQ(listing->entries[2].relative_path, "images/disk.hds");
    EXPECT_EQ(listing->entries[2].kind, axk::app::DirectoryEntryKind::file);
    EXPECT_EQ(listing->entries[2].size, 5U);
    EXPECT_FALSE(listing->truncated);

    const auto file = value.resolve_file({"workspace", "images/disk.hds"});
    ASSERT_TRUE(file) << file.error().message;
    EXPECT_EQ(*file, std::filesystem::canonical(root_ / "images" / "disk.hds"));
}

TEST_F(SandboxTest, ReenteringAWorkspaceRootRestartsDirectoryEnumeration) {
    const auto value = sandbox();
    const auto first_root = value.list_directory({"workspace", ""}, 20U);
    ASSERT_TRUE(first_root) << first_root.error().message;
    ASSERT_EQ(first_root->entries.size(), 1U);
    EXPECT_EQ(first_root->entries.front().name, "images");

    const auto child = value.list_directory({"workspace", "images"}, 20U);
    ASSERT_TRUE(child) << child.error().message;
    EXPECT_FALSE(child->entries.empty());

    const auto second_root = value.list_directory({"workspace", ""}, 20U);
    ASSERT_TRUE(second_root) << second_root.error().message;
    ASSERT_EQ(second_root->entries.size(), first_root->entries.size());
    EXPECT_EQ(second_root->entries.front().name, "images");
}

TEST_F(SandboxTest, InspectsOnlyTheSelectedDirectoryForSamplerObjects) {
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "images" / "objects"));
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "images" / "collection" / "nested"));
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "images" / "disk-set" / "DISK1"));
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "images" / "disk-set" / "DISK2"));
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "images" / "support-only"));
    std::ofstream(root_ / "images" / "objects" / "SMP_0001.001", std::ios::binary) << "FSFSDEV3SPLX";
    std::array<std::byte, 0x28U> segment_header{};
    std::ranges::copy(std::as_bytes(std::span{"FSFSDEV3SPLX", 12U}), segment_header.begin());
    std::ranges::copy(std::as_bytes(std::span{"SMPL", 4U}), segment_header.begin() + 0x0cU);
    segment_header[0x1fU] = std::byte{4U};
    segment_header[0x23U] = std::byte{2U};
    {
        std::ofstream segment_file{root_ / "images" / "disk-set" / "DISK1" / "SMP_0001.001", std::ios::binary};
        segment_file.write(reinterpret_cast<const char *>(segment_header.data()),
                           static_cast<std::streamsize>(segment_header.size()));
        ASSERT_TRUE(segment_file);
    }
    std::ofstream(root_ / "images" / "disk-set" / "DISK2" / "YAMAHA.SYM", std::ios::binary) << "support";
    std::ofstream(root_ / "images" / "objects" / "YAMAHA.SYM", std::ios::binary) << "support";
    std::ofstream(root_ / "images" / "support-only" / "YAMAHA.SYM", std::ios::binary) << "support";

    const auto value = sandbox();
    const auto ordinary = value.list_directory({"workspace", "images"}, 20U);
    ASSERT_TRUE(ordinary) << ordinary.error().message;
    const auto ordinary_objects = std::ranges::find(ordinary->entries, "objects", &axk::app::DirectoryEntry::name);
    ASSERT_NE(ordinary_objects, ordinary->entries.end());

    const auto objects = value.inspect_media_source({"workspace", "images/objects"});
    const auto collection = value.inspect_media_source({"workspace", "images/collection"});
    const auto disk_set = value.inspect_media_source({"workspace", "images/disk-set"});
    const auto support_only = value.inspect_media_source({"workspace", "images/support-only"});
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_TRUE(collection) << collection.error().message;
    ASSERT_TRUE(disk_set) << disk_set.error().message;
    ASSERT_TRUE(support_only) << support_only.error().message;
    EXPECT_EQ(objects->kind, axk::app::DirectoryMediaSourceKind::axk_object_directory);
    EXPECT_FALSE(collection->kind);
    EXPECT_EQ(disk_set->kind, axk::app::DirectoryMediaSourceKind::axk_object_directory);
    EXPECT_FALSE(support_only->kind);
    EXPECT_GT(objects->entries_visited, 0U);
    EXPECT_GT(objects->prefixes_read, 0U);
}

TEST_F(SandboxTest, SupportsAnEmptyRootSetAndAtomicReplacement) {
    auto value = axk::app::Sandbox::create({});
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_TRUE(value->roots().empty());
    EXPECT_FALSE(value->resolve_file({"workspace", "images/disk.hds"}));

    auto shared = *value;
    ASSERT_TRUE(value->replace_roots({{"workspace", "Workspace", root_, true}}));
    ASSERT_EQ(shared.roots().size(), 1U);
    EXPECT_TRUE(shared.resolve_file({"workspace", "images/disk.hds"}));

    ASSERT_TRUE(shared.replace_roots({}));
    EXPECT_TRUE(value->roots().empty());
}

TEST_F(SandboxTest, PagesDirectoriesDeterministicallyWithOpaqueCursor) {
    const auto value = sandbox();
    const auto first = value.list_directory({"workspace", "images"}, 2U);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_EQ(first->entries.size(), 2U);
    EXPECT_EQ(first->entries[0].name, "folder");
    EXPECT_EQ(first->entries[1].name, "alpha.hds");
    ASSERT_TRUE(first->truncated);
    ASSERT_TRUE(first->next_cursor);

    const auto second = value.list_directory({"workspace", "images"}, 2U, *first->next_cursor);
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_EQ(second->entries.size(), 2U);
    EXPECT_EQ(second->entries[0].name, "disk.hds");
    EXPECT_EQ(second->entries[1].name, "zulu.hds");
    EXPECT_FALSE(second->truncated);
    EXPECT_FALSE(second->next_cursor);

    const auto invalid = value.list_directory({"workspace", "images"}, 2U, "not-a-cursor");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, "invalid_file_reference");
}

TEST_F(SandboxTest, AcceptsThePublicDirectoryPageLimitAndRejectsLargerValues) {
    const auto value = sandbox();
    const auto maximum = value.list_directory({"workspace", "images"}, 5000U);
    ASSERT_TRUE(maximum) << maximum.error().message;
    const auto excessive = value.list_directory({"workspace", "images"}, 5001U);
    ASSERT_FALSE(excessive);
    EXPECT_EQ(excessive.error().code, "invalid_file_reference");
}

TEST_F(SandboxTest, ResolvesMetadataAndWritableOutputsWithoutAcceptingAliases) {
    const auto value = sandbox();
    const auto metadata = value.metadata("workspace", "images/disk.hds");
    ASSERT_TRUE(metadata) << metadata.error().message;
    EXPECT_EQ(metadata->kind, axk::app::DirectoryEntryKind::file);
    EXPECT_EQ(metadata->size, 5U);
    EXPECT_TRUE(metadata->writable);

    const auto output = value.resolve_output_file({"workspace", "images/new.hds"}, false);
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(*output, std::filesystem::canonical(root_ / "images") / "new.hds");

    const auto existing = value.resolve_output_file({"workspace", "images/disk.hds"}, false);
    ASSERT_FALSE(existing);
    EXPECT_EQ(existing.error().code, "output_exists");
    const auto replacement = value.resolve_output_file({"workspace", "images/disk.hds"}, true);
    ASSERT_TRUE(replacement) << replacement.error().message;

    const auto alias = value.require_distinct({"workspace", "images/disk.hds"}, {"workspace", "images/disk.hds"});
    ASSERT_FALSE(alias);
    EXPECT_EQ(alias.error().code, "invalid_file_reference");
}

TEST_F(SandboxTest, RetainsWritableFileIdentityAcrossBoundedMutation) {
    const auto value = sandbox();
    const auto mutation = value.open_mutation({"workspace", "images/disk.hds"});
    ASSERT_TRUE(mutation) << mutation.error().message;
    EXPECT_EQ((*mutation)->size(), 5U);
    ASSERT_TRUE((*mutation)->verify_bound());
    const std::array replacement{std::byte{'I'}, std::byte{'M'}};
    ASSERT_TRUE((*mutation)->write_exact_at(0U, replacement));
    ASSERT_TRUE((*mutation)->flush());
    ASSERT_TRUE((*mutation)->verify_bound());
    std::array<std::byte, 2> read{};
    ASSERT_TRUE((*mutation)->read_exact_at(0U, read));
    EXPECT_EQ(read, replacement);
    EXPECT_FALSE((*mutation)->write_exact_at(4U, replacement));
}

TEST_F(SandboxTest, ExposesAnOpaqueRevisionThatChangesWithRetainedFileContent) {
    const auto value = sandbox();
    const auto first = value.open_file({"workspace", "images/disk.hds"});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_EQ(first->revision.size(), 64U);
    EXPECT_TRUE(std::ranges::all_of(first->revision, [](unsigned char character) {
        return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
    }));

    std::ofstream(root_ / "images" / "disk.hds", std::ios::binary | std::ios::trunc) << "changed";
    EXPECT_FALSE(first->verify_unchanged());
    const auto second = value.open_file({"workspace", "images/disk.hds"});
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second->revision, first->revision);
}

#if defined(_WIN32)
TEST_F(SandboxTest, PreservesFileContentRevisionAcrossMetadataOnlyChangeTimeUpdates) {
    const auto value = sandbox();
    const auto first = value.open_file({"workspace", "images/disk.hds"});
    ASSERT_TRUE(first) << first.error().message;

    const auto close_handle = [](void *handle) {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    };
    std::unique_ptr<void, decltype(close_handle)> metadata{
        CreateFileW((root_ / "images" / "disk.hds").c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr),
        close_handle};
    ASSERT_NE(metadata.get(), INVALID_HANDLE_VALUE);

    FILE_BASIC_INFO before{};
    ASSERT_NE(GetFileInformationByHandleEx(metadata.get(), FileBasicInfo, &before, sizeof(before)), 0);
    FILE_BASIC_INFO update{};
    update.ChangeTime.QuadPart = before.ChangeTime.QuadPart + 10'000'000;
    ASSERT_NE(SetFileInformationByHandle(metadata.get(), FileBasicInfo, &update, sizeof(update)), 0);
    FILE_BASIC_INFO after{};
    ASSERT_NE(GetFileInformationByHandleEx(metadata.get(), FileBasicInfo, &after, sizeof(after)), 0);
    ASSERT_NE(after.ChangeTime.QuadPart, before.ChangeTime.QuadPart);
    ASSERT_EQ(after.LastWriteTime.QuadPart, before.LastWriteTime.QuadPart);
    metadata.reset();

    ASSERT_TRUE(first->verify_unchanged());
    const auto second = value.open_file({"workspace", "images/disk.hds"});
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second->revision, first->revision);
}
#endif

TEST_F(SandboxTest, RejectsOutputsInReadOnlyRootsAndEscapingParents) {
    const auto read_only = axk::app::Sandbox::create({{"workspace", "Workspace", root_, false}});
    ASSERT_TRUE(read_only) << read_only.error().message;
    EXPECT_FALSE(read_only->resolve_output_file({"workspace", "images/new.hds"}, false));
    EXPECT_FALSE(sandbox().resolve_output_file({"workspace", "outside/new.hds"}, false));
}

TEST_F(SandboxTest, RejectsUnknownRootsAbsoluteTraversalAndAlternateSeparators) {
    const auto value = sandbox();
    for (const auto &reference :
         {axk::app::FileRef{"missing", "images/disk.hds"}, axk::app::FileRef{"workspace", "../secret.hds"},
          axk::app::FileRef{"workspace", "/etc/passwd"}, axk::app::FileRef{"workspace", "images\\disk.hds"}}) {
        const auto result = value.resolve_file(reference);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error().code, "invalid_file_reference");
    }
}

TEST_F(SandboxTest, UsesTheHostFilesystemsCaseSemanticsWithoutGuessing) {
    const auto mixed_case_path = root_ / "images" / "MixedCase.hds";
    const auto alternate_case_path = root_ / "images" / "mixedcase.hds";
    std::ofstream(mixed_case_path) << "case";
    const auto host_resolves_alternate_case = std::filesystem::exists(alternate_case_path);

    const auto resolved = sandbox().resolve_file({"workspace", "images/mixedcase.hds"});
    EXPECT_EQ(resolved.has_value(), host_resolves_alternate_case);
    if (resolved) {
        EXPECT_TRUE(std::filesystem::equivalent(*resolved, mixed_case_path));
    }
}

TEST_F(SandboxTest, RejectsPortableReservedPathComponents) {
    const auto value = sandbox();
    for (const auto &relative_path :
         {"images/CON", "images/nul.txt", "images/COM1.hds", "images/trailing. ", "images/control\x01.hds"}) {
        const auto result = value.resolve_output_file({"workspace", relative_path}, false);
        EXPECT_FALSE(result) << relative_path;
        EXPECT_EQ(result.error().code, "invalid_file_reference");
    }
}

TEST_F(SandboxTest, RejectsHardLinkAliasesAsSourceDestinations) {
    std::error_code error;
    std::filesystem::create_hard_link(root_ / "images" / "disk.hds", root_ / "images" / "alias.hds", error);
    if (error)
        GTEST_SKIP() << "hard links are unavailable: " << error.message();

    const auto distinct =
        sandbox().require_distinct({"workspace", "images/disk.hds"}, {"workspace", "images/alias.hds"});
    ASSERT_FALSE(distinct);
    EXPECT_EQ(distinct.error().code, "invalid_file_reference");
}

TEST_F(SandboxTest, RejectsSymlinksThatEscapeTheConfiguredRoot) {
    std::error_code error;
    std::filesystem::create_directory_symlink(outside_, root_ / "outside", error);
    if (error)
        GTEST_SKIP() << "directory symlinks are unavailable: " << error.message();

    const auto result = sandbox().resolve_file({"workspace", "outside/secret.hds"});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "invalid_file_reference");
}

TEST_F(SandboxTest, RejectsSymlinksEvenWhenTheirCurrentTargetRemainsInsideTheRoot) {
    std::error_code error;
    std::filesystem::create_directory_symlink(root_ / "images", root_ / "linked-images", error);
    if (error)
        GTEST_SKIP() << "directory symlinks are unavailable: " << error.message();

    const auto value = sandbox();
    EXPECT_FALSE(value.resolve_file({"workspace", "linked-images/disk.hds"}));
    EXPECT_FALSE(value.resolve_output_file({"workspace", "linked-images/new.hds"}, false));
}

TEST_F(SandboxTest, OpenFileRetainsTheValidatedObjectAcrossAParentSwap) {
    const auto value = sandbox();
    const auto opened = value.open_file({"workspace", "images/folder/inside.txt"});
    ASSERT_FALSE(opened);

    std::ofstream(root_ / "images" / "folder" / "inside.txt") << "inside";
    const auto retained = value.open_file({"workspace", "images/folder/inside.txt"});
    ASSERT_TRUE(retained) << retained.error().message;

    const auto parent = root_ / "images" / "folder";
    const auto parked = root_ / "images" / "folder-parked";
    std::error_code error;
    std::filesystem::rename(parent, parked, error);
#if defined(_WIN32)
    if (error == std::errc::permission_denied)
        GTEST_SKIP() << "Windows prevented the parent swap while the retained file handle was open";
#endif
    ASSERT_FALSE(error) << error.message();
    std::ofstream(outside_ / "inside.txt") << "outside";
    std::filesystem::create_directory_symlink(outside_, parent, error);
    if (error) {
        std::filesystem::rename(parked, parent, error);
        GTEST_SKIP() << "directory links are unavailable";
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(retained->size));
    ASSERT_TRUE(retained->reader->read_exact_at(0U, bytes));
    const std::string text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    EXPECT_EQ(text, "inside");

    std::filesystem::remove(parent, error);
    std::filesystem::rename(parked, parent, error);
    ASSERT_FALSE(error) << error.message();
}

TEST_F(SandboxTest, OpenTreeReopensValidatedObjectsRelativeToTheRetainedRoot) {
    const auto value = sandbox();
    std::ofstream(root_ / "images" / "folder" / "inside.txt") << "inside";
    const auto retained = value.open_tree({"workspace", "images/folder"}, {8U, 1024U});
    ASSERT_TRUE(retained) << retained.error().message;
    ASSERT_EQ(retained->entries().size(), 1U);

    const auto parent = root_ / "images" / "folder";
    const auto parked = root_ / "images" / "folder-parked";
    std::error_code error;
    std::filesystem::rename(parent, parked, error);
#if defined(_WIN32)
    if (error == std::errc::permission_denied)
        GTEST_SKIP() << "Windows prevented the parent swap while the retained directory handle was open";
#endif
    ASSERT_FALSE(error) << error.message();
    std::ofstream(outside_ / "inside.txt") << "outside";
    std::filesystem::create_directory_symlink(outside_, parent, error);
    if (error) {
        std::filesystem::rename(parked, parent, error);
        GTEST_SKIP() << "directory links are unavailable";
    }

    {
        auto opened = retained->open_file(0U);
        ASSERT_TRUE(opened) << opened.error().message;
        std::vector<std::byte> bytes(static_cast<std::size_t>(retained->entries().front().size));
        ASSERT_TRUE(opened->reader->read_exact_at(0U, bytes));
        ASSERT_TRUE(opened->verify_unchanged());
        EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), "inside");
    }

    std::filesystem::remove(parent, error);
    std::filesystem::rename(parked, parent, error);
    ASSERT_FALSE(error) << error.message();
}

TEST_F(SandboxTest, OpenTreeRejectsAFileReplacedAfterCollection) {
    const auto value = sandbox();
    std::ofstream(root_ / "images" / "original.txt") << "original";
    const auto tree = value.open_tree({"workspace", "images"}, {8U, 1024U});
    ASSERT_TRUE(tree) << tree.error().message;
    const auto found = std::ranges::find(tree->entries(), "original.txt", &axk::app::SandboxTreeEntry::relative_path);
    ASSERT_NE(found, tree->entries().end());
    const auto index = static_cast<std::size_t>(std::distance(tree->entries().begin(), found));

    std::filesystem::rename(root_ / "images" / "original.txt", root_ / "images" / "parked.txt");
    std::ofstream(root_ / "images" / "original.txt") << "replacement";

    const auto opened = tree->open_file(index);
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, "archive_source_changed");
}

TEST_F(SandboxTest, OpenTreeBoundsDepthAndAggregatePathBytes) {
    const auto value = sandbox();
    std::filesystem::create_directories(root_ / "images/deep/child");
    std::ofstream(root_ / "images/deep/child/file.txt") << "data";

    const auto too_deep = value.open_tree({"workspace", "images"}, {16U, 1024U, 2U, 1024U});
    ASSERT_FALSE(too_deep);
    EXPECT_EQ(too_deep.error().code, "download_archive_too_large");

    const auto too_many_path_bytes = value.open_tree({"workspace", "images"}, {16U, 1024U, 8U, 8U});
    ASSERT_FALSE(too_many_path_bytes);
    EXPECT_EQ(too_many_path_bytes.error().code, "download_archive_too_large");
}

TEST_F(SandboxTest, RejectsMutationsAfterAValidatedParentIsReplacedByALink) {
    const auto value = sandbox();
    const auto parent = root_ / "images" / "folder";
    const auto parked = root_ / "images" / "folder-parked";
    std::error_code error;
    std::filesystem::rename(parent, parked, error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::create_directory_symlink(outside_, parent, error);
    if (error) {
        const auto link_error = error;
        error.clear();
        std::filesystem::rename(parked, parent, error);
        ASSERT_FALSE(error) << error.message();
        GTEST_SKIP() << "directory links are unavailable: " << link_error.message();
    }

    EXPECT_FALSE(value.create_directory({"workspace", "images/folder"}, "escaped"));
    EXPECT_FALSE(value.rename_entry({"workspace", "images/folder/secret.hds"}, "renamed.hds"));
    EXPECT_FALSE(value.delete_entry({"workspace", "images/folder/secret.hds"}));
    EXPECT_EQ(std::filesystem::file_size(outside_ / "secret.hds"), 6U);
    EXPECT_FALSE(std::filesystem::exists(outside_ / "escaped"));
    EXPECT_FALSE(std::filesystem::exists(outside_ / "renamed.hds"));

    std::filesystem::remove(parent, error);
    std::filesystem::rename(parked, parent, error);
    ASSERT_FALSE(error) << error.message();
}

TEST_F(SandboxTest, CreatesRenamesAndDeletesWritableEntriesWithoutOverwriting) {
    const auto value = sandbox();

    const auto created = value.create_directory({"workspace", "images"}, "New Folder");
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->relative_path, "images/New Folder");
    EXPECT_EQ(created->kind, axk::app::DirectoryEntryKind::directory);
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "images" / "New Folder"));

    const auto renamed = value.rename_entry({"workspace", "images/New Folder"}, "Renamed Folder");
    ASSERT_TRUE(renamed) << renamed.error().message;
    EXPECT_EQ(renamed->relative_path, "images/Renamed Folder");
    EXPECT_FALSE(std::filesystem::exists(root_ / "images" / "New Folder"));
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "images" / "Renamed Folder"));

    ASSERT_TRUE(value.delete_entry({"workspace", "images/Renamed Folder"}));
    EXPECT_FALSE(std::filesystem::exists(root_ / "images" / "Renamed Folder"));
    ASSERT_TRUE(value.delete_entry({"workspace", "images/alpha.hds"}));
    EXPECT_FALSE(std::filesystem::exists(root_ / "images" / "alpha.hds"));
}

TEST_F(SandboxTest, RejectsUnsafeEntryMutationsAndPreservesExistingData) {
    const auto value = sandbox();
    std::ofstream(root_ / "images" / "folder" / "keep.txt") << "keep";

    for (const auto &name : {"", ".", "..", "nested/name", "nested\\name", "CON", "trailing."}) {
        const auto created = value.create_directory({"workspace", "images"}, name);
        EXPECT_FALSE(created) << name;
        EXPECT_EQ(created.error().code, "invalid_file_reference");
    }

    const auto collision = value.rename_entry({"workspace", "images/alpha.hds"}, "disk.hds");
    ASSERT_FALSE(collision);
    EXPECT_EQ(collision.error().code, "output_exists");
    EXPECT_TRUE(std::filesystem::exists(root_ / "images" / "alpha.hds"));
    EXPECT_EQ(std::filesystem::file_size(root_ / "images" / "disk.hds"), 5U);

    const auto nonempty = value.delete_entry({"workspace", "images/folder"});
    ASSERT_FALSE(nonempty);
    EXPECT_EQ(nonempty.error().code, "directory_not_empty");
    EXPECT_TRUE(std::filesystem::exists(root_ / "images" / "folder" / "keep.txt"));

    EXPECT_FALSE(value.rename_entry({"workspace", ""}, "renamed-root"));
    EXPECT_FALSE(value.delete_entry({"workspace", ""}));

    const auto read_only = axk::app::Sandbox::create({{"workspace", "Workspace", root_, false}});
    ASSERT_TRUE(read_only) << read_only.error().message;
    EXPECT_EQ(read_only->create_directory({"workspace", "images"}, "blocked").error().code, "read_only_root");
    EXPECT_EQ(read_only->rename_entry({"workspace", "images/alpha.hds"}, "blocked.hds").error().code, "read_only_root");
    EXPECT_EQ(read_only->delete_entry({"workspace", "images/alpha.hds"}).error().code, "read_only_root");
}

#if !defined(_WIN32)
TEST_F(SandboxTest, ParentSwapCannotRedirectFilePublicationOutsideTheRoot) {
    const auto value = sandbox();
    const auto parent = root_ / "images" / "folder";
    const auto parked = root_ / "images" / "folder-parked";
    const axk::MemoryReader content{
        {std::byte{0x69}, std::byte{0x6e}, std::byte{0x73}, std::byte{0x69}, std::byte{0x64}, std::byte{0x65}}};
    std::atomic_bool stop_attacker{};
    std::thread attacker{[&] {
        while (!stop_attacker.load(std::memory_order_relaxed)) {
            std::error_code error;
            std::filesystem::rename(parent, parked, error);
            if (error)
                continue;
            std::filesystem::create_directory_symlink(outside_, parent, error);
            if (!error)
                std::this_thread::yield();
            std::filesystem::remove(parent, error);
            std::filesystem::rename(parked, parent, error);
        }
    }};
    const ThreadStopGuard stop_and_join{stop_attacker, attacker};

    for (std::size_t attempt = 0; attempt < 1000U; ++attempt) {
        const auto published = value.publish_file({"workspace", "images/folder/published.bin"}, true, content);
        if (published) {
            static_cast<void>(value.delete_entry({"workspace", "images/folder/published.bin"}));
        }
        EXPECT_FALSE(std::filesystem::exists(outside_ / "published.bin"));
    }
    EXPECT_FALSE(std::filesystem::exists(outside_ / "published.bin"));
}

TEST_F(SandboxTest, PublishesAStagedTreeWithoutFollowingASwappedParent) {
#if defined(_WIN32)
    GTEST_SKIP() << "the adversarial directory swap uses POSIX symbolic links";
#else
    const auto value = sandbox();
    const auto staging = std::filesystem::temp_directory_path() / "axklib-sandbox-tree-staging";
    std::error_code error;
    std::filesystem::remove_all(staging, error);
    ASSERT_TRUE(std::filesystem::create_directories(staging / "nested", error));
    ASSERT_FALSE(error);
    std::ofstream(staging / "nested" / "result.txt") << "inside";

    const auto initial = value.publish_directory({"workspace", "images/initial"}, false, staging);
    ASSERT_TRUE(initial) << initial.error().message;
    EXPECT_EQ(read_text(root_ / "images" / "initial" / "nested" / "result.txt"), "inside");

    std::filesystem::rename(root_ / "images", root_ / "images-held", error);
    ASSERT_FALSE(error);
    std::filesystem::create_directory_symlink(outside_, root_ / "images", error);
    ASSERT_FALSE(error);

    const auto published = value.publish_directory({"workspace", "images/result"}, false, staging);
    EXPECT_FALSE(published);
    EXPECT_FALSE(std::filesystem::exists(outside_ / "result"));
    std::filesystem::remove_all(staging, error);
#endif
}

TEST_F(SandboxTest, ParentSwapCannotRedirectMutationsOutsideTheRoot) {
    const auto value = sandbox();
    const auto parent = root_ / "images" / "folder";
    const auto parked = root_ / "images" / "folder-parked";
    std::ofstream(parent / "rename-source.hds") << "inside";
    std::ofstream(outside_ / "rename-source.hds") << "outside";
    std::atomic_bool stop_attacker{};
    std::thread attacker{[&] {
        while (!stop_attacker.load(std::memory_order_relaxed)) {
            std::error_code error;
            std::filesystem::rename(parent, parked, error);
            if (error)
                continue;
            std::filesystem::create_directory_symlink(outside_, parent, error);
            if (!error)
                std::this_thread::yield();
            std::filesystem::remove(parent, error);
            std::filesystem::rename(parked, parent, error);
        }
    }};
    const ThreadStopGuard stop_and_join{stop_attacker, attacker};

    for (std::size_t attempt = 0; attempt < 2000U; ++attempt) {
        const auto created = value.create_directory({"workspace", "images/folder"}, "escaped");
        if (created)
            static_cast<void>(value.delete_entry({"workspace", "images/folder/escaped"}));
        const auto renamed = value.rename_entry({"workspace", "images/folder/rename-source.hds"}, "renamed.hds");
        if (renamed) {
            bool restored = false;
            for (std::size_t retry = 0; retry < 10000U && !restored; ++retry) {
                restored =
                    value.rename_entry({"workspace", "images/folder/renamed.hds"}, "rename-source.hds").has_value();
                if (!restored)
                    std::this_thread::yield();
            }
            ASSERT_TRUE(restored);
        }
        EXPECT_FALSE(std::filesystem::exists(outside_ / "escaped"));
        EXPECT_FALSE(std::filesystem::exists(outside_ / "renamed.hds"));
        std::ifstream outside_source{outside_ / "rename-source.hds"};
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>{outside_source}, {}), "outside");
    }
    EXPECT_FALSE(std::filesystem::exists(outside_ / "escaped"));
}
#endif

TEST_F(SandboxTest, RequiresUniqueValidExistingDirectoryRoots) {
    auto duplicate =
        axk::app::Sandbox::create({{"workspace", "First", root_, true}, {"workspace", "Second", outside_, true}});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "invalid_sandbox_root");

    auto file_root = axk::app::Sandbox::create({{"workspace", "File", root_ / "images" / "disk.hds", true}});
    ASSERT_FALSE(file_root);
    EXPECT_EQ(file_root.error().code, "invalid_sandbox_root");
}

TEST_F(SandboxTest, PreservesUnregisteredPublicationLikeWorkspaceEntries) {
    const auto abandoned_file = root_ / "images" / ".result.axklib-publication.p4294967295.1.tmp";
    const auto abandoned_directory = root_ / "images" / ".export.axklib-publication.p4294967295.2.tmp";
    const auto ordinary_file = root_ / "images" / ".result.tmp";
    std::ofstream(abandoned_file) << "partial";
    std::filesystem::create_directory(abandoned_directory);
    std::ofstream(abandoned_directory / "partial.wav") << "partial";
    std::ofstream(ordinary_file) << "ordinary";

    const auto removed = sandbox().cleanup_abandoned_publications();
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_EQ(*removed, 0U);
    EXPECT_TRUE(std::filesystem::exists(abandoned_file));
    EXPECT_TRUE(std::filesystem::exists(abandoned_directory));
    EXPECT_TRUE(std::filesystem::exists(abandoned_directory / "partial.wav"));
    EXPECT_TRUE(std::filesystem::exists(ordinary_file));
}

TEST_F(SandboxTest, PreservesPublicationSiblingsOwnedByTheCurrentProcess) {
#if defined(_WIN32)
    const auto process_id = static_cast<unsigned long long>(::_getpid());
#else
    const auto process_id = static_cast<unsigned long long>(::getpid());
#endif
    const auto active_file =
        root_ / "images" / (".result.axklib-publication.p" + std::to_string(process_id) + ".1.tmp");
    std::ofstream(active_file) << "active";

    const auto removed = sandbox().cleanup_abandoned_publications();
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_EQ(*removed, 0U);
    EXPECT_TRUE(std::filesystem::exists(active_file));
}

} // namespace
