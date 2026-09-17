#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../../../library/tests/media_ex5_fixture.hpp"
#include "axklib/application/filesystem_edit_operations.hpp"
#include "axklib/application/system_file_operations.hpp"
#include "content_digest.hpp"

namespace {
class FatFilesystemEdits : public testing::TestWithParam<std::string> {
  protected:
    std::filesystem::path root;
    std::unique_ptr<axk::app::Sandbox> sandbox;
    axk::app::PathReservationCoordinator reservations;
    std::unique_ptr<axk::app::ImageSessionManager> sessions;
    axk::app::ImageSessionSummary opened;
    std::vector<std::byte> original;
    axk::PartitionIndex partition{};
    std::size_t selected_begin{};
    std::size_t selected_size{};

    void SetUp() override {
        original = ex5_fixture();
        if (GetParam() == "ex5-mo-capacity") {
            original = ex5_capacity_fixture(false, true);
        } else if (GetParam() != "ex5-hd") {
            original.erase(original.begin(), original.begin() + boot_offset);
            ascii(original, 3U, GetParam() == "ex5-mo" ? "YAMAHA??" : "MSDOS5.0");
            le16(original, 19U, 0U);
            le32(original, 32U, static_cast<std::uint32_t>(original.size() / sector_bytes));
        }
        selected_size = original.size();
        if (GetParam() == "mbr") {
            const auto volume = std::move(original);
            constexpr std::size_t start = 63U * sector_bytes;
            original.assign(start + 2U * volume.size() + sector_bytes, std::byte{});
            original[510U] = std::byte{0x55};
            original[511U] = std::byte{0xaa};
            for (std::size_t ordinal = 0; ordinal < 2U; ++ordinal) {
                const auto offset = start + ordinal * volume.size();
                const auto slot = ordinal * 2U;
                original[446U + slot * 16U + 4U] = std::byte{0x06};
                le32(original, 446U + slot * 16U + 8U, static_cast<std::uint32_t>(offset / sector_bytes));
                le32(original, 446U + slot * 16U + 12U, static_cast<std::uint32_t>(volume.size() / sector_bytes));
                std::ranges::copy(volume, original.begin() + static_cast<std::ptrdiff_t>(offset));
            }
            partition = axk::PartitionIndex{1};
            selected_begin = start + volume.size();
        }
        root = std::filesystem::temp_directory_path() /
               std::format("axk-fat-edits-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root / "workspace");
        {
            std::ofstream output{root / "workspace/image.hda", std::ios::binary};
            output.write(reinterpret_cast<const char *>(original.data()),
                         static_cast<std::streamsize>(original.size()));
            ASSERT_TRUE(output);
        }
        auto created = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
        ASSERT_TRUE(created);
        sandbox = std::make_unique<axk::app::Sandbox>(std::move(*created));
        sessions = std::make_unique<axk::app::ImageSessionManager>(*sandbox, 32U, 500U, std::chrono::minutes{15},
                                                                   std::chrono::steady_clock::now, &reservations);
        auto result = sessions->open({"workspace", "image.hda"}, "owner");
        ASSERT_TRUE(result) << result.error().message;
        opened = std::move(*result);
    }
    void TearDown() override {
        sessions.reset();
        sandbox.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::string digest() const { return axk::app::detail::file_sha256(root / "workspace/image.hda").value(); }
    static std::vector<axk::FilesystemEdit> edits() {
        return {
            axk::CreateFilesystemDirectory{{"NEW"}},
            axk::PutFilesystemFile{{"NEW", "EMPTY.BIN"}, std::make_shared<axk::MemoryReader>(std::vector<std::byte>{})},
            axk::PutFilesystemFile{
                {"NEW", "DATA.BIN"},
                std::make_shared<axk::MemoryReader>(std::vector<std::byte>(10001U, std::byte{0x57}))}};
    }
    axk::app::Result<axk::app::ImageSessionSummary>
    apply(axk::app::AlterationJournalStore &journals, const axk::CancellationToken &cancellation = {},
          const std::function<axk::app::Result<void>()> &validate = {}) {
        return axk::app::apply_filesystem_edits(*sessions, journals, opened.image_id, "owner", opened.revision,
                                                partition, edits(), cancellation, nullptr, {{}, validate});
    }
};

TEST_P(FatFilesystemEdits, RejectsSystemParameterEditsAndReleasesMutationAccess) {
    const auto before = digest();
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto result = axk::app::apply_system_file(*sessions, journals, opened.image_id, "owner", opened.revision,
                                                    partition, {}, axk::ASeriesModel::a4000);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "image_mutation_unsupported");
    EXPECT_EQ(digest(), before);
    const auto read = sessions->begin_read(opened.image_id, "owner", opened.revision);
    EXPECT_TRUE(read);
}

TEST_P(FatFilesystemEdits, RenamesPopulatedDirectoriesThroughTheRegisteredJobWithoutChangingData) {
    const auto roots = sessions->filesystem(opened.image_id, "owner", opened.revision);
    ASSERT_TRUE(roots);
    const auto root_id = roots->items.back().id;
    const auto children = sessions->filesystem(opened.image_id, "owner", opened.revision, {.parent_id = root_id});
    ASSERT_TRUE(children);
    const auto directory = std::ranges::find(children->items, "DEMOS", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(directory, children->items.end());
    const auto directory_id = directory->id;
    const std::vector<axk::app::ImageFilesystemEdit> mixed{
        axk::app::RenameImageFilesystemEntry{directory_id, "RENAMED"},
        axk::app::RemoveImageFilesystemEntry{directory_id, true}};
    EXPECT_FALSE(sessions->resolve_filesystem_edits(opened.image_id, "owner", opened.revision, mixed));
    axk::app::AlterationJournalStore journals{root / "journals"};
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, *sessions, journals));
    const axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "rename", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const nlohmann::json request{
        {"imageId", opened.image_id},
        {"expectedRevision", opened.revision},
        {"acknowledgeDeviceRelationships", true},
        {"edits", nlohmann::json::array({{{"kind", "RENAME"}, {"entryId", directory_id}, {"newName", "Renamed"}}})}};
    const auto result = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->at("warnings").empty());
    const auto renamed =
        sessions->filesystem(opened.image_id, "owner", opened.revision + 1U, {.entry_id = directory_id});
    ASSERT_TRUE(renamed);
    ASSERT_EQ(renamed->items.size(), 1U);
    EXPECT_EQ(renamed->items.front().name, "RENAMED");
    const auto descendants =
        sessions->filesystem(opened.image_id, "owner", opened.revision + 1U, {.parent_id = directory_id});
    ASSERT_TRUE(descendants);
    EXPECT_TRUE(std::ranges::any_of(descendants->items, [](const auto &entry) {
        return entry.name == "DEMO1.S1A" && entry.path == "/RENAMED/DEMO1.S1A";
    }));
    const auto source = axk::FileReader::open(root / "workspace/image.hda").value();
    std::vector<std::byte> after(original.size());
    ASSERT_TRUE(source->read_exact_at(0U, after));
    std::size_t differences{};
    for (std::size_t i = 0; i < original.size(); ++i)
        differences += static_cast<std::size_t>(original[i] != after[i]);
    EXPECT_LE(differences, 11U);
    EXPECT_EQ(source->size(), original.size());
    const auto committed_digest = digest();
    EXPECT_FALSE(registry.invoke("images.filesystem.edit", request, context));
    EXPECT_EQ(digest(), committed_digest);
}

TEST_P(FatFilesystemEdits, ComputesContentFingerprintsOnDemandAndInvalidatesAfterEdits) {
    const auto before = digest();
    {
        auto read = sessions->begin_read(opened.image_id, "owner", opened.revision);
        ASSERT_TRUE(read);
        ASSERT_EQ(read->content_fingerprint({}).value(), before);
        EXPECT_EQ(read->content_fingerprint({}).value(), before);
        axk::CancellationSource cancellation;
        cancellation.cancel();
        EXPECT_FALSE(read->content_fingerprint(cancellation.token()));
    }
    axk::app::AlterationJournalStore journals{root / "journals"};
    auto changed = apply(journals);
    ASSERT_TRUE(changed) << changed.error().message;
    auto read = sessions->begin_read(opened.image_id, "owner", changed->revision);
    ASSERT_TRUE(read);
    const auto after = read->content_fingerprint({});
    ASSERT_TRUE(after);
    EXPECT_NE(*after, before);
    EXPECT_EQ(*after, digest());
    const auto path = root / "workspace/image.hda";
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds{1});
    EXPECT_FALSE(read->content_fingerprint({}));
}

TEST_P(FatFilesystemEdits, DelegatesReviewedInputsButRetainsDefaultDigestChecks) {
    class CountingReader final : public axk::RandomAccessReader {
      public:
        axk::MemoryReader source{std::vector<std::byte>(10001U, std::byte{0x57})};
        mutable std::uint64_t bytes_read{};
        std::uint64_t size() const noexcept override { return source.size(); }
        axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> output) const override {
            bytes_read += output.size();
            return source.read_exact_at(offset, output);
        }
    };
    auto input = std::make_shared<CountingReader>();
    std::filesystem::copy_file(root / "workspace/image.hda", root / "workspace/second.hda");
    const auto second = sessions->open({"workspace", "second.hda"}, "owner");
    ASSERT_TRUE(second);
    axk::app::AlterationJournalStore journals{root / "journals"};
    const std::vector<axk::FilesystemEdit> first{axk::PutFilesystemFile{{"FIRST.BIN"}, input}};
    auto changed = axk::app::apply_filesystem_edits(*sessions, journals, opened.image_id, "owner", opened.revision,
                                                    partition, first);
    ASSERT_TRUE(changed) << changed.error().message;
    const auto default_bytes = input->bytes_read;
    input->bytes_read = 0U;
    std::size_t verification_count{};
    const axk::app::FilesystemInputVerification verified{{input}, [&]() -> axk::app::Result<void> {
                                                             ++verification_count;
                                                             return {};
                                                         }};
    changed = axk::app::apply_filesystem_edits(*sessions, journals, second->image_id, "owner", second->revision,
                                               partition, first, {}, nullptr, verified);
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(verification_count, 1U);
    EXPECT_EQ(default_bytes, input->bytes_read + 2U * input->size());
    EXPECT_GE(input->bytes_read, input->size());
    const auto invalid =
        axk::app::apply_filesystem_edits(*sessions, journals, second->image_id, "owner", changed->revision, partition,
                                         first, {}, nullptr, {{input}, {}});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, "invalid_request");
}

TEST_P(FatFilesystemEdits, RejectsIdenticalReplacementBeforeMutation) {
    const auto path = root / "workspace/image.hda";
    std::filesystem::rename(path, root / "workspace/original.hda");
    std::filesystem::copy_file(root / "workspace/original.hda", path);
    const auto before = digest();
    axk::app::AlterationJournalStore journals{root / "journals"};
    EXPECT_FALSE(apply(journals));
    EXPECT_EQ(digest(), before);
}

TEST_P(FatFilesystemEdits, CancellationDuringCommitRefreshRestoresTheOriginalRevision) {
    class CancelOnRefresh final : public axk::ProgressSink {
      public:
        axk::CancellationSource cancellation;
        void report(const axk::Progress &progress) noexcept override {
            if (progress.label == "Refreshing image metadata")
                cancellation.cancel();
        }
    } progress;
    const auto before = digest();
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto changed =
        axk::app::apply_filesystem_edits(*sessions, journals, opened.image_id, "owner", opened.revision, partition,
                                         edits(), progress.cancellation.token(), &progress);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, "operation_cancelled");
    EXPECT_EQ(digest(), before);
    EXPECT_TRUE(journals.storage_ready());
    auto read = sessions->begin_read(opened.image_id, "owner", opened.revision);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->content_fingerprint({}).value(), before);
}

TEST_P(FatFilesystemEdits, PrewriteSourceChangesDoNotRefreshAStaleSession) {
    class ChangeOnPublish final : public axk::ProgressSink {
      public:
        std::filesystem::path path;
        void report(const axk::Progress &progress) noexcept override {
            if (progress.label == "Committing filesystem changes") {
                std::error_code ignored;
                const auto time = std::filesystem::last_write_time(path, ignored);
                std::filesystem::last_write_time(path, time + std::chrono::seconds{1}, ignored);
            }
        }
    } progress;
    progress.path = root / "workspace/image.hda";
    const auto before = digest();
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto changed = axk::app::apply_filesystem_edits(*sessions, journals, opened.image_id, "owner",
                                                          opened.revision, partition, edits(), {}, &progress);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, "image_source_changed");
    EXPECT_EQ(digest(), before);
    EXPECT_TRUE(journals.storage_ready());
    EXPECT_FALSE(sessions->begin_read(opened.image_id, "owner", opened.revision));
}

TEST_P(FatFilesystemEdits, PublishesRawEditsWithoutEnablingSamplerObjectMutations) {
    if (GetParam() == "ex5-mo-capacity") {
        EXPECT_EQ(opened.validation.warning_count, 1U);
        const auto issues = sessions->validation_issues(opened.image_id, "owner", 20U);
        ASSERT_TRUE(issues);
        ASSERT_EQ(issues->items.size(), 1U);
        EXPECT_EQ(issues->items[0].code, "EX5_CAPACITY_EXCEEDS_IMAGE");
    }
    EXPECT_FALSE(sessions->begin_mutation(opened.image_id, "owner", opened.revision));
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto result = apply(journals);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->revision, opened.revision + 1U);
    EXPECT_EQ(std::filesystem::file_size(root / "workspace/image.hda"), original.size());
    if (GetParam() == "ex5-mo-capacity")
        EXPECT_EQ(result->validation.warning_count, 1U);
    const auto read = sessions->begin_read(opened.image_id, "owner", result->revision);
    ASSERT_TRUE(read) << read.error().message;
    const auto *fat = std::get_if<axk::FatImage>(&read->media->storage());
    if (const auto *disk = std::get_if<axk::FatDiskImage>(&read->media->storage())) {
        ASSERT_EQ(disk->partitions().at(1U).number, 3U);
        fat = &disk->partitions().at(1U).volume;
    }
    ASSERT_NE(fat, nullptr);
    const auto data = std::ranges::find(fat->files(), "NEW/DATA.BIN", &axk::FatFile::path);
    ASSERT_NE(data, fat->files().end());
    EXPECT_EQ(fat->read_file(*data).value(), std::vector<std::byte>(10001U, std::byte{0x57}));
    const auto retained = std::ranges::find(fat->files(), "DEMOS/DEMO1.S1A", &axk::FatFile::path);
    ASSERT_NE(retained, fat->files().end());
    const auto content = fat->read_file(*retained).value();
    EXPECT_TRUE(std::ranges::all_of(std::span{content}.first(512U), [](auto b) { return b == std::byte{0x31}; }));
    EXPECT_TRUE(std::ranges::all_of(std::span{content}.subspan(512U), [](auto b) { return b == std::byte{0x72}; }));
    auto source = axk::FileReader::open(root / "workspace/image.hda").value();
    std::vector<std::byte> after(original.size());
    ASSERT_TRUE(source->read_exact_at(0U, after));
    EXPECT_TRUE(
        std::equal(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(selected_begin), after.begin()));
    EXPECT_TRUE(std::equal(original.begin() + static_cast<std::ptrdiff_t>(selected_begin + selected_size),
                           original.end(),
                           after.begin() + static_cast<std::ptrdiff_t>(selected_begin + selected_size)));
}

TEST_P(FatFilesystemEdits, CancelledPartialCommitRollsBackAndCanBeRetriedAtTheSameRevision) {
    const auto before = digest();
    axk::CancellationSource cancellation;
    bool reached{};
    axk::app::AlterationJournalStore journals{root / "journals", 32U * 1024U * 1024U,
                                              [&](std::string_view phase, std::size_t) {
                                                  if (phase == "after-patch-chunk") {
                                                      reached = true;
                                                      cancellation.cancel();
                                                  }
                                                  return false;
                                              },
                                              127U};
    const auto cancelled = apply(journals, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_TRUE(reached);
    EXPECT_EQ(cancelled.error().code, "operation_cancelled");
    EXPECT_EQ(digest(), before);
    EXPECT_TRUE(journals.storage_ready());
    {
        EXPECT_TRUE(sessions->begin_read(opened.image_id, "owner", opened.revision));
    }
    const auto retry = apply(journals);
    EXPECT_TRUE(retry) << retry.error().message;
}

TEST_P(FatFilesystemEdits, FailedPostWriteValidationRollsBackAndCanBeRetried) {
    const auto before = digest();
    axk::app::AlterationJournalStore journals{root / "journals"};
    bool validated{};
    const auto failed = apply(journals, {}, [&]() -> axk::app::Result<void> {
        validated = true;
        return std::unexpected(axk::app::Error{"filesystem_input_changed", "Injected changed-input failure"});
    });
    ASSERT_FALSE(failed);
    EXPECT_TRUE(validated);
    EXPECT_EQ(failed.error().code, "filesystem_input_changed");
    EXPECT_EQ(digest(), before);
    const auto retry = apply(journals);
    EXPECT_TRUE(retry) << retry.error().message;
}

TEST_P(FatFilesystemEdits, RejectsExternallyChangedSourceAndOtherOwners) {
    axk::app::AlterationJournalStore journals{root / "journals"};
    EXPECT_FALSE(axk::app::apply_filesystem_edits(*sessions, journals, opened.image_id, "other", opened.revision,
                                                  partition, edits()));
    const auto path = root / "workspace/image.hda";
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds{1});
    const auto before = digest();
    const auto failed = apply(journals);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, "image_source_changed");
    EXPECT_EQ(digest(), before);
}

TEST_P(FatFilesystemEdits, InterruptedPartialWriteRecoversBeforeOpeningANewSession) {
    const auto before = digest();
    axk::app::AlterationJournalStore journals{
        root / "journals", 32U * 1024U * 1024U,
        [](std::string_view phase, std::size_t index) { return phase == "after-patch-chunk" && index == 0U; }, 127U};
    const auto failed = apply(journals);
    ASSERT_FALSE(failed);
    EXPECT_FALSE(journals.storage_ready());
    EXPECT_NE(digest(), before);
    sessions.reset();
    axk::app::AlterationJournalStore recovered{root / "journals"};
    const auto restored = recovered.recover(*sandbox);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(digest(), before);
    EXPECT_TRUE(recovered.storage_ready());
    sessions = std::make_unique<axk::app::ImageSessionManager>(*sandbox, 32U, 500U, std::chrono::minutes{15},
                                                               std::chrono::steady_clock::now, &reservations);
    const auto reopened = sessions->open({"workspace", "image.hda"}, "owner");
    ASSERT_TRUE(reopened) << reopened.error().message;
    opened = *reopened;
    const auto retry = apply(recovered);
    EXPECT_TRUE(retry) << retry.error().message;
}

TEST_P(FatFilesystemEdits, ResolvesReviewsAndRunsRegisteredJobsWithTheCorrectRootIdentity) {
    using Json = nlohmann::json;
    const auto roots = sessions->filesystem(opened.image_id, "owner", 1U);
    ASSERT_TRUE(roots);
    const auto root_id = roots->items.back().id;
    const auto capability =
        std::ranges::find(roots->root_capabilities, root_id, &axk::app::ImageFilesystemRootCapabilities::root_id);
    ASSERT_NE(capability, roots->root_capabilities.end());
    EXPECT_TRUE(capability->create_directory);
    EXPECT_TRUE(capability->put_file);
    EXPECT_TRUE(capability->delete_entry);
    EXPECT_TRUE(capability->rename_entry);
    EXPECT_EQ(capability->maximum_name_bytes, 12U);
    EXPECT_EQ(capability->name_policy, "FAT_8_3_UPPERCASE");
    const std::vector<axk::app::ImageFilesystemEdit> requests{
        axk::app::CreateImageFilesystemDirectory{root_id, {"NEW"}}};
    const auto resolved = sessions->resolve_filesystem_edits(opened.image_id, "owner", 1U, requests);
    ASSERT_TRUE(resolved) << resolved.error().message;
    EXPECT_EQ(resolved->partition, partition);
    if (GetParam() == "mbr") {
        const std::vector<axk::app::ImageFilesystemEdit> mixed{
            requests.front(), axk::app::CreateImageFilesystemDirectory{roots->items.front().id, {"NEW"}}};
        EXPECT_FALSE(sessions->resolve_filesystem_edits(opened.image_id, "owner", 1U, mixed));
    }
    axk::app::AlterationJournalStore journals{root / "journals"};
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, *sessions, journals));
    const axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const std::vector<axk::FilesystemImportEntry> entries{{{"NEW"}, true, 0U}, {{"NEW", "DATA.BIN"}, false, 4U}};
    const auto review = sessions->inspect_filesystem_import(opened.image_id, "owner", 1U, root_id, entries);
    ASSERT_TRUE(review) << review.error().message;
    ASSERT_EQ(review->size(), 2U);
    EXPECT_EQ(review->at(0).action, axk::FilesystemImportAction::create_directory);
    EXPECT_EQ(review->at(1).action, axk::FilesystemImportAction::create_file);
    const Json review_request{
        {"imageId", opened.image_id},
        {"expectedRevision", 1U},
        {"parentEntryId", root_id},
        {"entries",
         Json::array({{{"relativePath", {"NEW"}}, {"directory", true}, {"sizeBytes", 0U}, {"conflict", "SKIP"}},
                      {{"relativePath", {"NEW", "DATA.BIN"}},
                       {"directory", false},
                       {"sizeBytes", 4U},
                       {"conflict", "SKIP"}}})}};
    const auto job_review = registry.invoke("images.filesystem.import.inspect", review_request, context);
    ASSERT_TRUE(job_review) << job_review.error().message;
    EXPECT_EQ(job_review->at("entries")[0].at("action"), "CREATE_DIRECTORY");
    EXPECT_EQ(job_review->at("entries")[1].at("action"), "CREATE_FILE");
    {
        std::ofstream file{root / "workspace/input.bin", std::ios::binary};
        file << "1234";
        ASSERT_TRUE(file);
    }
    const Json source{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "input.bin"}}}};
    const auto inputs = registry.invoke("filesystem.inputs.inspect", {{"inputs", Json::array({source})}}, context);
    ASSERT_TRUE(inputs) << inputs.error().message;
    Json request{
        {"imageId", opened.image_id},
        {"expectedRevision", 1U},
        {"acknowledgeDeviceRelationships", true},
        {"edits", Json::array({{{"kind", "CREATE_DIRECTORY"}, {"parentEntryId", root_id}, {"relativePath", {"NEW"}}},
                               {{"kind", "PUT_FILE"},
                                {"parentEntryId", root_id},
                                {"relativePath", {"NEW", "DATA.BIN"}},
                                {"source", source},
                                {"expectedSource", inputs->at("inputs")[0].at("snapshot")}}})}};
    const auto committed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_TRUE(committed) << committed.error().message;
    EXPECT_EQ(committed->at("warnings"), nlohmann::json::array());
    EXPECT_EQ(committed->at("revision"), 2U);
    const auto children = sessions->filesystem(opened.image_id, "owner", 2U, {.parent_id = root_id});
    ASSERT_TRUE(children);
    const auto directory = std::ranges::find(children->items, "NEW", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(directory, children->items.end());
    const auto again = sessions->inspect_filesystem_import(opened.image_id, "owner", 2U, root_id, entries);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->at(0).action, axk::FilesystemImportAction::merge_directory);
    EXPECT_EQ(again->at(1).action, axk::FilesystemImportAction::skip_file);
    const auto before = digest();
    EXPECT_FALSE(registry.invoke("images.filesystem.edit", request, context));
    request["expectedRevision"] = 2U;
    request["edits"] = Json::array({{{"kind", "DELETE"}, {"entryId", directory->id}, {"recursive", false}}});
    EXPECT_FALSE(registry.invoke("images.filesystem.edit", request, context));
    EXPECT_EQ(digest(), before);
    request["edits"][0]["recursive"] = true;
    const auto removed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_EQ(removed->at("revision"), 3U);
}

TEST_P(FatFilesystemEdits, UnsafeFatRemainsReadableWithoutAdvertisingOrResolvingWrites) {
    sessions.reset();
    const auto volume = axk::FatImage::open(std::make_shared<axk::MemoryReader>(
        std::vector<std::byte>{original.begin() + static_cast<std::ptrdiff_t>(selected_begin),
                               original.begin() + static_cast<std::ptrdiff_t>(selected_begin + selected_size)}));
    ASSERT_TRUE(volume) << volume.error().message;
    const auto fat = selected_begin + volume->geometry().fat_offset;
    const auto table_bytes =
        static_cast<std::uint64_t>(volume->geometry().sectors_per_fat) * volume->geometry().bytes_per_sector;
    // A cleared clean-shutdown bit must not be repaired by an ordinary Files edit.
    le16(original, fat + 2U, 0x7fffU);
    le16(original, fat + table_bytes + 2U, 0x7fffU);
    {
        std::ofstream file{root / "workspace/image.hda", std::ios::binary};
        file.write(reinterpret_cast<const char *>(original.data()), static_cast<std::streamsize>(original.size()));
        ASSERT_TRUE(file);
    }
    sessions = std::make_unique<axk::app::ImageSessionManager>(*sandbox, 32U, 500U, std::chrono::minutes{15},
                                                               std::chrono::steady_clock::now, &reservations);
    const auto result = sessions->open({"workspace", "image.hda"}, "owner");
    ASSERT_TRUE(result) << result.error().message;
    const auto roots = sessions->filesystem(result->image_id, "owner", result->revision);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto root_id = roots->items.back().id;
    const auto capability =
        std::ranges::find(roots->root_capabilities, root_id, &axk::app::ImageFilesystemRootCapabilities::root_id);
    ASSERT_NE(capability, roots->root_capabilities.end());
    EXPECT_FALSE(capability->create_directory);
    EXPECT_FALSE(capability->put_file);
    EXPECT_FALSE(capability->delete_entry);
    EXPECT_FALSE(capability->rename_entry);
    const std::vector<axk::app::ImageFilesystemEdit> edits{axk::app::CreateImageFilesystemDirectory{root_id, {"NEW"}}};
    EXPECT_FALSE(sessions->resolve_filesystem_edits(result->image_id, "owner", result->revision, edits));
    const auto children = sessions->filesystem(result->image_id, "owner", result->revision, {.parent_id = root_id});
    ASSERT_TRUE(children);
    EXPECT_FALSE(children->items.empty());
    if (GetParam() == "mbr") {
        EXPECT_TRUE(roots->root_capabilities.front().create_directory);
    }
}

TEST_P(FatFilesystemEdits, ReadOnlyWorkspaceDoesNotAdvertiseWrites) {
    sessions.reset();
    auto read_only = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", false}});
    ASSERT_TRUE(read_only);
    sandbox = std::make_unique<axk::app::Sandbox>(std::move(*read_only));
    sessions = std::make_unique<axk::app::ImageSessionManager>(*sandbox, 32U, 500U, std::chrono::minutes{15},
                                                               std::chrono::steady_clock::now, &reservations);
    const auto result = sessions->open({"workspace", "image.hda"}, "owner");
    ASSERT_TRUE(result);
    const auto roots = sessions->filesystem(result->image_id, "owner", result->revision);
    ASSERT_TRUE(roots);
    for (const auto &capability : roots->root_capabilities) {
        EXPECT_FALSE(capability.create_directory);
        EXPECT_FALSE(capability.put_file);
        EXPECT_FALSE(capability.delete_entry);
        EXPECT_FALSE(capability.rename_entry);
    }
}

INSTANTIATE_TEST_SUITE_P(Filesystems, FatFilesystemEdits,
                         testing::Values("ex5-hd", "ex5-mo", "ex5-mo-capacity", "fat16", "mbr"));
} // namespace
