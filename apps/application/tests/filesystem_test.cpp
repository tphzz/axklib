#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "axklib/application/filesystem.hpp"

namespace {

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

TEST_F(SandboxTest, RequiresUniqueValidExistingDirectoryRoots) {
    auto duplicate =
        axk::app::Sandbox::create({{"workspace", "First", root_, true}, {"workspace", "Second", outside_, true}});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "invalid_sandbox_root");

    auto file_root = axk::app::Sandbox::create({{"workspace", "File", root_ / "images" / "disk.hds", true}});
    ASSERT_FALSE(file_root);
    EXPECT_EQ(file_root.error().code, "invalid_sandbox_root");
}

TEST_F(SandboxTest, RemovesOnlyAbandonedAxklibPublicationSiblings) {
    const auto abandoned_file = root_ / "images" / ".result.axklib-publication.p4294967295.1.tmp";
    const auto abandoned_directory = root_ / "images" / ".export.axklib-publication.p4294967295.2.tmp";
    const auto ordinary_file = root_ / "images" / ".result.tmp";
    std::ofstream(abandoned_file) << "partial";
    std::filesystem::create_directory(abandoned_directory);
    std::ofstream(abandoned_directory / "partial.wav") << "partial";
    std::ofstream(ordinary_file) << "ordinary";

    const auto removed = sandbox().cleanup_abandoned_publications();
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_EQ(*removed, 2U);
    EXPECT_FALSE(std::filesystem::exists(abandoned_file));
    EXPECT_FALSE(std::filesystem::exists(abandoned_directory));
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
