#include <array>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "axklib/application/alteration_journal.hpp"

namespace {

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(AlterationJournalStoreTest, AppliesPreparedPatchesAndRemovesCommittedJournal) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-test";
    const auto workspace = root / "workspace";
    const auto journals = root / "journals";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(workspace);
    std::ofstream(workspace / "image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", workspace, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::AlterationJournalStore store{journals};
    ASSERT_TRUE(store.storage_ready());
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target) << target.error().message;

    std::vector<axk::app::AlterationJournalPatch> patches;
    patches.push_back({2U, {std::byte{'2'}, std::byte{'3'}}, {std::byte{'A'}, std::byte{'B'}}});
    ASSERT_TRUE(store.apply(*target, 10U, patches));
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "01AB456789");
    EXPECT_TRUE(std::filesystem::is_empty(journals));
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, RejectsStaleOriginalBytesWithoutWriting) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-stale-test";
    const auto workspace = root / "workspace";
    const auto journals = root / "journals";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(workspace);
    std::ofstream(workspace / "image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", workspace, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::AlterationJournalStore store{journals};
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target) << target.error().message;
    std::vector<axk::app::AlterationJournalPatch> patches;
    patches.push_back({2U, {std::byte{'X'}}, {std::byte{'A'}}});
    EXPECT_FALSE(store.apply(*target, 10U, patches));
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "0123456789");
    EXPECT_TRUE(std::filesystem::is_empty(journals));
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, RecoversOriginalBytesAfterInterruptedPartialWrite) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-recovery-test";
    const auto workspace = root / "workspace";
    const auto journals = root / "journals";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(workspace);
    std::ofstream(workspace / "image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", workspace, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::AlterationJournalStore interrupted{
        journals, 1024U * 1024U,
        [](std::string_view phase, std::size_t index) { return phase == "after-patch" && index == 0U; }};
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target) << target.error().message;
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}}, {std::byte{'A'}}},
        axk::app::AlterationJournalPatch{7U, {std::byte{'7'}}, {std::byte{'B'}}},
    };

    EXPECT_FALSE(interrupted.apply(*target, 10U, patches));
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "01A3456789");
    EXPECT_FALSE(std::filesystem::is_empty(journals));

    axk::app::AlterationJournalStore recovered{journals};
    ASSERT_TRUE(recovered.recover(*sandbox));
    EXPECT_EQ(read_text(workspace / "image.hds"), "0123456789");
    EXPECT_TRUE(std::filesystem::is_empty(journals));
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, PreservesCommittedBytesAfterInterruptedCleanup) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-commit-test";
    const auto workspace = root / "workspace";
    const auto journals = root / "journals";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(workspace);
    std::ofstream(workspace / "image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", workspace, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::AlterationJournalStore interrupted{
        journals, 1024U * 1024U, [](std::string_view phase, std::size_t) { return phase == "after-commit-marker"; }};
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target) << target.error().message;
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}}, {std::byte{'A'}}},
    };

    EXPECT_FALSE(interrupted.apply(*target, 10U, patches));
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "01A3456789");
    EXPECT_FALSE(std::filesystem::is_empty(journals));

    axk::app::AlterationJournalStore recovered{journals};
    ASSERT_TRUE(recovered.recover(*sandbox));
    EXPECT_EQ(read_text(workspace / "image.hds"), "01A3456789");
    EXPECT_TRUE(std::filesystem::is_empty(journals));
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, RemovesAnOrphanCommitMarkerDuringRecovery) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-orphan-marker-test";
    const auto workspace = root / "workspace";
    const auto journals = root / "journals";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(workspace);
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", workspace, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::AlterationJournalStore store{journals};
    ASSERT_TRUE(store.storage_ready());
    std::ofstream(journals / "alteration-orphan.axkjournal.commit", std::ios::binary) << "orphan";

    ASSERT_TRUE(store.recover(*sandbox));
    EXPECT_TRUE(std::filesystem::is_empty(journals));
    std::filesystem::remove_all(root, error);
}

} // namespace
