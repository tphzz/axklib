#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/writer.hpp"

namespace {

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool journal_state_empty(const std::filesystem::path &path) {
    for (const auto &entry : std::filesystem::directory_iterator{path}) {
        if (entry.path().filename() != ".axklib-publication")
            return false;
        if (!entry.is_directory() || !std::filesystem::is_empty(entry.path()))
            return false;
    }
    return true;
}

class BoundedJournalReader final : public axk::RandomAccessReader {
  public:
    explicit BoundedJournalReader(std::uint64_t count, std::byte value) : count_(count), value_(value) {}
    std::uint64_t size() const noexcept override { return count_; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        largest_read = std::max(largest_read, destination.size());
        if (destination.size() > 4096U || offset > count_ || destination.size() > count_ - offset ||
            bytes_read + destination.size() > count_)
            return std::unexpected(axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                                                   "journal reader exceeded its single bounded pass"));
        bytes_read += destination.size();
        std::ranges::fill(destination, value_);
        return {};
    }
    mutable std::size_t largest_read{};
    mutable std::uint64_t bytes_read{};

  private:
    std::uint64_t count_;
    std::byte value_;
};

TEST(AlterationJournalStoreTest, StreamsReaderBackedPatchesAndAppliesOnlyFrozenJournalBytes) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-stream-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    constexpr std::size_t size = 2U * 1024U * 1024U + 17U;
    {
        std::ofstream output{root / "workspace/image.hds", std::ios::binary};
        const std::string block(4096U, '0');
        for (std::size_t offset = 0; offset < size; offset += block.size())
            output.write(block.data(), static_cast<std::streamsize>(std::min(block.size(), size - offset)));
    }
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    auto input = std::make_shared<BoundedJournalReader>(size - 2U, std::byte{'X'});
    const std::array patches{axk::app::AlterationJournalPatch{1U,
                                                              axk::app::AlterationJournalBytes{*target, 1U, size - 2U},
                                                              axk::app::AlterationJournalBytes{input, 0U, size - 2U}}};
    axk::app::AlterationJournalStore store{root / "journals", 8U * 1024U * 1024U, {}, 4096U};
    const auto applied = store.apply(*target, size, patches);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_LE(input->largest_read, 4096U);
    EXPECT_EQ(input->bytes_read, size - 2U);
    EXPECT_EQ(read_text(root / "workspace/image.hds"), "0" + std::string(size - 2U, 'X') + "0");
    EXPECT_TRUE(journal_state_empty(root / "journals"));
    target = {};
}

TEST(AlterationJournalStoreTest, RejectsOverlappingPatchesBeforeWriting) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-overlap-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    std::ofstream(root / "workspace/image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}, std::byte{'3'}}, {std::byte{'A'}, std::byte{'B'}}},
        axk::app::AlterationJournalPatch{3U, {std::byte{'3'}}, {std::byte{'C'}}}};
    axk::app::AlterationJournalStore store{root / "journals"};
    EXPECT_FALSE(store.apply(*target, 10U, patches));
    EXPECT_EQ(read_text(root / "workspace/image.hds"), "0123456789");
    EXPECT_TRUE(journal_state_empty(root / "journals"));
}

TEST(AlterationJournalStoreTest, FailedRollbackReadbackRetainsJournalAndBlocksWrites) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-rollback-readback-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    const auto path = root / "workspace/image.hds";
    std::ofstream(path, std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}, std::byte{'3'}}, {std::byte{'A'}, std::byte{'B'}}}};
    axk::app::AlterationJournalStore store{root / "journals", 4096U, [&](std::string_view phase, std::size_t) {
                                               if (phase == "after-rollback-flush") {
                                                   std::fstream file{path,
                                                                     std::ios::binary | std::ios::in | std::ios::out};
                                                   file.seekp(2);
                                                   file.put('X');
                                               }
                                               return false;
                                           }};
    const auto failed = store.apply(*target, 10U, patches, {}, []() -> axk::app::Result<void> {
        return std::unexpected(axk::app::Error{"test_failure", "Reject commit"});
    });
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().message, "alteration target does not match its journal");
    EXPECT_FALSE(store.storage_ready());
    EXPECT_FALSE(journal_state_empty(root / "journals"));
    EXPECT_FALSE(store.apply(*target, 10U, patches));
}

TEST(AlterationJournalStoreTest, CancellationDuringValidationRollsBackBeforeTheCommitMarker) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-validation-cancel-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    const auto path = root / "workspace/image.hds";
    std::ofstream(path, std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}, std::byte{'3'}}, {std::byte{'A'}, std::byte{'B'}}}};
    axk::app::AlterationJournalStore store{root / "journals"};
    axk::CancellationSource cancellation;
    const auto failed = store.apply(*target, 10U, patches, cancellation.token(), [&]() -> axk::app::Result<void> {
        cancellation.cancel();
        return {};
    });
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, "operation_cancelled");
    EXPECT_EQ(read_text(path), "0123456789");
    EXPECT_TRUE(store.storage_ready());
    EXPECT_TRUE(journal_state_empty(root / "journals"));
}

TEST(AlterationJournalStoreTest, CancelsStreamedCommitAndRestoresTheFrozenOriginal) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-stream-cancel-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    std::ofstream(root / "workspace/image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    const std::array patches{axk::app::AlterationJournalPatch{
        2U, axk::app::AlterationJournalBytes{*target, 2U, 4U},
        axk::app::AlterationJournalBytes{std::make_shared<BoundedJournalReader>(4U, std::byte{'X'}), 0U, 4U}}};
    axk::CancellationSource cancellation;
    axk::app::AlterationJournalStore store{root / "journals", 4096U,
                                           [&](std::string_view phase, std::size_t index) {
                                               if (phase == "after-patch-chunk" && index == 0U)
                                                   cancellation.cancel();
                                               return false;
                                           },
                                           2U};
    const auto applied = store.apply(*target, 10U, patches, cancellation.token());
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, "operation_cancelled");
    EXPECT_EQ(read_text(root / "workspace/image.hds"), "0123456789");
    EXPECT_TRUE(store.storage_ready());
    EXPECT_TRUE(journal_state_empty(root / "journals"));
}

TEST(AlterationJournalStoreTest, RecoversStreamedOriginalsAfterAnInterruptedWrite) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-stream-recover-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    std::ofstream(root / "workspace/image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    axk::app::AlterationJournalStore store{
        root / "journals", 4096U,
        [](std::string_view phase, std::size_t index) { return phase == "after-patch-chunk" && index == 0U; }, 2U};
    {
        auto target = sandbox->open_mutation({"workspace", "image.hds"});
        ASSERT_TRUE(target);
        const std::array patches{axk::app::AlterationJournalPatch{
            2U, axk::app::AlterationJournalBytes{*target, 2U, 4U},
            axk::app::AlterationJournalBytes{std::make_shared<BoundedJournalReader>(4U, std::byte{'X'}), 0U, 4U}}};
        EXPECT_FALSE(store.apply(*target, 10U, patches));
    }
    EXPECT_EQ(read_text(root / "workspace/image.hds"), "01XX456789");
    const auto recovered = store.recover(*sandbox);
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(read_text(root / "workspace/image.hds"), "0123456789");
    EXPECT_TRUE(journal_state_empty(root / "journals"));
}

TEST(AlterationJournalStoreTest, RejectsInvalidAndFailingReaderRangesWithoutWriting) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-stream-input-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    std::ofstream(root / "workspace/image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    auto failing = std::make_shared<BoundedJournalReader>(4U, std::byte{'X'});
    failing->bytes_read = 4U;
    const std::array inputs{axk::app::AlterationJournalBytes{nullptr, 0U, 4U},
                            axk::app::AlterationJournalBytes{*target, 9U, 4U},
                            axk::app::AlterationJournalBytes{failing, 0U, 4U}};
    axk::app::AlterationJournalStore store{root / "journals"};
    for (const auto &input : inputs) {
        const std::array patches{
            axk::app::AlterationJournalPatch{2U, axk::app::AlterationJournalBytes{*target, 2U, 4U}, input}};
        EXPECT_FALSE(store.apply(*target, 10U, patches));
        EXPECT_EQ(read_text(root / "workspace/image.hds"), "0123456789");
        EXPECT_TRUE(journal_state_empty(root / "journals"));
        EXPECT_TRUE(store.storage_ready());
    }
}

TEST(AlterationJournalStoreTest, QuarantinesAnUnexpectedExceptionUntilRecovery) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-exception-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    std::ofstream(root / "workspace/image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    axk::app::AlterationJournalStore store{root / "journals"};
    {
        auto target = sandbox->open_mutation({"workspace", "image.hds"});
        ASSERT_TRUE(target);
        const std::array patches{axk::app::AlterationJournalPatch{2U, {std::byte{'2'}}, {std::byte{'A'}}}};
        EXPECT_THROW(static_cast<void>(store.apply(*target, 10U, patches, {},
                                                   []() -> axk::app::Result<void> {
                                                       throw std::runtime_error("injected validation exception");
                                                   })),
                     std::runtime_error);
        EXPECT_FALSE(store.storage_ready());
        EXPECT_FALSE(store.apply(*target, 10U, patches));
    }
    ASSERT_TRUE(store.recover(*sandbox));
    EXPECT_EQ(read_text(root / "workspace/image.hds"), "0123456789");
    EXPECT_TRUE(store.storage_ready());
    EXPECT_TRUE(journal_state_empty(root / "journals"));
}

TEST(AlterationJournalStoreTest, DefaultLimitCoversTheSupportedImageBoundary) {
    constexpr auto metadata_allowance = 64ULL * 1024ULL * 1024ULL;
    constexpr auto maximum_image_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    EXPECT_EQ(axk::maximum_hds_size, maximum_image_bytes);
    EXPECT_EQ(axk::app::default_maximum_alteration_journal_bytes, maximum_image_bytes * 2U + metadata_allowance);
}

TEST(AlterationJournalStoreTest, AppliesAndRecoversTinyPatchesAboveFourGiB) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-high-offset-test";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    const auto path = root / "workspace/image.hds";
    constexpr std::uint64_t image_size = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t patch_offset = 7ULL * 1024ULL * 1024ULL * 1024ULL + 123U;
    {
        std::ofstream image{path, std::ios::binary};
        image.seekp(static_cast<std::streamoff>(image_size - 1U));
        image.put('\0');
        image.seekp(static_cast<std::streamoff>(patch_offset));
        image.write("AB", 2);
        ASSERT_TRUE(image);
    }
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    const auto read_patch = [&]() {
        std::ifstream image{path, std::ios::binary};
        image.seekg(static_cast<std::streamoff>(patch_offset));
        std::string bytes(2U, '\0');
        image.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        EXPECT_TRUE(image);
        return bytes;
    };
    {
        auto target = sandbox->open_mutation({"workspace", "image.hds"});
        ASSERT_TRUE(target) << target.error().message;
        const std::array patches{axk::app::AlterationJournalPatch{
            patch_offset, {std::byte{'A'}, std::byte{'B'}}, {std::byte{'C'}, std::byte{'D'}}}};
        axk::app::AlterationJournalStore store{root / "journals"};
        const auto applied = store.apply(*target, image_size, patches);
        ASSERT_TRUE(applied) << applied.error().message;
        EXPECT_TRUE(journal_state_empty(root / "journals"));
    }
    EXPECT_EQ(read_patch(), "CD");
    axk::app::AlterationJournalStore interrupted{
        root / "journals", axk::app::default_maximum_alteration_journal_bytes,
        [](std::string_view phase, std::size_t index) { return phase == "after-patch-chunk" && index == 0U; }, 1U};
    {
        auto target = sandbox->open_mutation({"workspace", "image.hds"});
        ASSERT_TRUE(target) << target.error().message;
        const std::array patches{axk::app::AlterationJournalPatch{
            patch_offset, {std::byte{'C'}, std::byte{'D'}}, {std::byte{'E'}, std::byte{'F'}}}};
        EXPECT_FALSE(interrupted.apply(*target, image_size, patches));
    }
    EXPECT_EQ(read_patch(), "ED");
    const auto recovered = interrupted.recover(*sandbox);
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(read_patch(), "CD");
    EXPECT_EQ(std::filesystem::file_size(path), image_size);
    EXPECT_TRUE(journal_state_empty(root / "journals"));
}

TEST(AlterationJournalStoreTest, ReportsExactCapacityBeforeWritingTheTarget) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-capacity-test";
    const auto workspace = root / "workspace";
    const auto journals = root / "journals";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(workspace);
    std::ofstream(workspace / "image.hds", std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", workspace, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::AlterationJournalStore store{journals, 1U};
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target) << target.error().message;
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}, std::byte{'3'}}, {std::byte{'A'}, std::byte{'B'}}},
    };

    const auto applied = store.apply(*target, 10U, patches);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, "alteration_journal_capacity");
    EXPECT_NE(applied.error().message.find("requires"), std::string::npos);
    EXPECT_NE(applied.error().message.find("configured limit"), std::string::npos);
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "0123456789");
    EXPECT_TRUE(journal_state_empty(journals));
    std::filesystem::remove_all(root, error);
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
    EXPECT_TRUE(journal_state_empty(journals));
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
    EXPECT_TRUE(journal_state_empty(journals));
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, RejectsSourceRevisionChangesWhileFreezingTheJournal) {
    class ChangingReader final : public axk::RandomAccessReader {
      public:
        std::filesystem::path path;
        std::uint64_t size() const noexcept override { return 1U; }
        axk::Result<void> read_exact_at(std::uint64_t, std::span<std::byte> output) const override {
            std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds{1});
            std::ranges::fill(output, std::byte{'A'});
            return {};
        }
    };
    const auto root = std::filesystem::temp_directory_path() / "axklib-journal-source-revision-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "workspace");
    const auto path = root / "workspace/image.hds";
    std::ofstream(path, std::ios::binary) << "0123456789";
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    auto input = std::make_shared<ChangingReader>();
    input->path = path;
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}}, axk::app::AlterationJournalBytes{input, 0U, 1U}}};
    axk::app::AlterationJournalStore store{root / "journals"};
    const auto result = store.apply(*target, 10U, patches);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "image_source_changed");
    EXPECT_EQ(read_text(path), "0123456789");
    EXPECT_TRUE(store.storage_ready());
    EXPECT_TRUE(journal_state_empty(root / "journals"));
    target = {};
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
    EXPECT_FALSE(interrupted.storage_ready());
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "01A3456789");
    EXPECT_FALSE(journal_state_empty(journals));

    ASSERT_TRUE(interrupted.recover(*sandbox));
    EXPECT_TRUE(interrupted.storage_ready());
    EXPECT_EQ(read_text(workspace / "image.hds"), "0123456789");
    EXPECT_TRUE(journal_state_empty(journals));
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, RecoversOriginalBytesAfterInterruptionWithinAMultiBytePatch) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-torn-patch-test";
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
        [](std::string_view phase, std::size_t index) { return phase == "after-patch-chunk" && index == 0U; }, 2U};
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target) << target.error().message;
    const std::array patches{
        axk::app::AlterationJournalPatch{
            2U,
            {std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'}},
            {std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'D'}},
        },
    };

    EXPECT_FALSE(interrupted.apply(*target, 10U, patches));
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "01AB456789");

    ASSERT_TRUE(interrupted.recover(*sandbox));
    EXPECT_EQ(read_text(workspace / "image.hds"), "0123456789");
    EXPECT_TRUE(journal_state_empty(journals));
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, RefusesToRecoverIntoAReplacementFile) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-identity-test";
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
    };

    EXPECT_FALSE(interrupted.apply(*target, 10U, patches));
    target = {};
    std::filesystem::rename(workspace / "image.hds", workspace / "displaced.hds");
    std::ofstream(workspace / "image.hds", std::ios::binary) << "replacement";

    const auto recovered = interrupted.recover(*sandbox);
    ASSERT_FALSE(recovered);
    EXPECT_EQ(recovered.error().code, "alteration_journal_unavailable");
    EXPECT_EQ(read_text(workspace / "image.hds"), "replacement");
    EXPECT_FALSE(interrupted.storage_ready());
    std::filesystem::remove_all(root, error);
}

TEST(AlterationJournalStoreTest, RollsBackBeforeCommitWhenSemanticValidationFails) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-journal-validation-test";
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
    const std::array patches{
        axk::app::AlterationJournalPatch{2U, {std::byte{'2'}}, {std::byte{'A'}}},
    };

    const auto validation = []() -> axk::app::Result<void> {
        return std::unexpected(axk::app::Error{"image_invalid", "patched image failed semantic validation"});
    };
    const auto applied = store.apply(*target, 10U, patches, {}, validation);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, "image_invalid");
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "0123456789");
    EXPECT_TRUE(journal_state_empty(journals));
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

    EXPECT_TRUE(interrupted.apply(*target, 10U, patches));
    EXPECT_FALSE(interrupted.storage_ready());
    target = {};
    EXPECT_EQ(read_text(workspace / "image.hds"), "01A3456789");
    EXPECT_FALSE(journal_state_empty(journals));

    ASSERT_TRUE(interrupted.recover(*sandbox));
    EXPECT_TRUE(interrupted.storage_ready());
    EXPECT_EQ(read_text(workspace / "image.hds"), "01A3456789");
    EXPECT_TRUE(journal_state_empty(journals));
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
    EXPECT_TRUE(journal_state_empty(journals));
    std::filesystem::remove_all(root, error);
}

} // namespace
