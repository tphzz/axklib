#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/filesystem_edit_operations.hpp"
#include "axklib/writer.hpp"
#include "content_digest.hpp"

namespace {
class MutableFileInput final : public axk::RandomAccessReader {
  public:
    std::byte value{0x21};
    std::uint64_t size() const noexcept override { return 4U; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (offset > size() || destination.size() > size() - offset)
            return std::unexpected(axk::make_error(axk::ErrorCode::io_short_read, axk::ErrorCategory::io,
                                                   "test input read exceeds its size"));
        std::ranges::fill(destination, value);
        return {};
    }
};

class FilesystemEditOperations : public testing::Test {
  protected:
    std::filesystem::path root;
    std::unique_ptr<axk::app::Sandbox> sandbox;
    axk::app::PathReservationCoordinator reservations;
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               std::format("axk-files-session-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root / "workspace");
        const axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {{"Files", {}}}};
        ASSERT_TRUE(axk::write_hds_image(manifest, root / "workspace/image.hds"));
        auto created = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
        ASSERT_TRUE(created);
        sandbox = std::make_unique<axk::app::Sandbox>(std::move(*created));
    }
    void TearDown() override {
        sandbox.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::string digest() const { return axk::app::detail::file_sha256(root / "workspace/image.hds").value(); }
    static std::vector<axk::FilesystemEdit> edits() {
        return {axk::CreateFilesystemDirectory{{"Documents"}},
                axk::PutFilesystemFile{
                    {"Documents", "file.bin"},
                    std::make_shared<axk::MemoryReader>(std::vector<std::byte>(10001U, std::byte{0x57}))}};
    }
};

TEST_F(FilesystemEditOperations, CommitsRawFilesAndRefreshesTheSessionRevision) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto updated = axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner",
                                                          opened->revision, axk::PartitionIndex{0}, edits());
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->revision, opened->revision + 1U);
    const auto roots = sessions.filesystem(opened->image_id, "owner", updated->revision);
    ASSERT_TRUE(roots);
    ASSERT_EQ(roots->items.size(), 1U);
    ASSERT_EQ(roots->root_capabilities.size(), 1U);
    EXPECT_EQ(roots->root_capabilities.front().root_id, roots->items.front().id);
    EXPECT_TRUE(roots->root_capabilities.front().create_directory);
    EXPECT_TRUE(roots->root_capabilities.front().put_file);
    EXPECT_TRUE(roots->root_capabilities.front().delete_entry);
    EXPECT_EQ(roots->root_capabilities.front().maximum_name_bytes, 23U);
    EXPECT_EQ(roots->root_capabilities.front().name_pattern, "^[ -~]{1,23}$");
    const auto files = sessions.filesystem(opened->image_id, "owner", updated->revision,
                                           {.root_id = roots->items.front().id, .query = "file.bin"});
    ASSERT_TRUE(files) << files.error().message;
    ASSERT_EQ(files->items.size(), 1U);
    EXPECT_EQ(files->items.front().size_bytes, 10001U);
    const auto image = axk::open_image(root / "workspace/image.hds");
    ASSERT_TRUE(image);
    const auto &records = image->partitions().front().records;
    const auto file = std::ranges::find(records, 10001U, &axk::IndexRecord::data_size);
    ASSERT_NE(file, records.end());
    const auto bytes = image->read_record_data(axk::PartitionIndex{0}, file->sfs_id, 10001U);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(*bytes, std::vector<std::byte>(10001U, std::byte{0x57}));
    const auto before = digest();
    const auto stale = axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                                        axk::PartitionIndex{0}, edits());
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "image_revision_stale");
    EXPECT_EQ(digest(), before);
}

TEST_F(FilesystemEditOperations, ReleasesMutationAccessAfterRejectedPlansAndCancellation) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto before = digest();
    const std::vector<axk::FilesystemEdit> invalid{axk::CreateFilesystemDirectory{{".."}}};
    EXPECT_FALSE(axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                                  axk::PartitionIndex{0}, invalid));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                                  axk::PartitionIndex{0}, edits(), cancellation.token()));
    EXPECT_EQ(digest(), before);
    const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
    EXPECT_TRUE(read);
}

TEST_F(FilesystemEditOperations, RejectsReadOnlySourcesAndOtherSessionsPathLeases) {
    auto readonly = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", false}});
    ASSERT_TRUE(readonly);
    axk::app::ImageSessionManager read_sessions{
        *readonly, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto read_image = read_sessions.open({"workspace", "image.hds"}, "reader");
    ASSERT_TRUE(read_image);
    const auto readonly_roots = read_sessions.filesystem(read_image->image_id, "reader", read_image->revision);
    ASSERT_TRUE(readonly_roots);
    ASSERT_EQ(readonly_roots->root_capabilities.size(), 1U);
    EXPECT_FALSE(readonly_roots->root_capabilities.front().create_directory);
    EXPECT_FALSE(readonly_roots->root_capabilities.front().put_file);
    EXPECT_FALSE(readonly_roots->root_capabilities.front().delete_entry);
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto before = digest();
    EXPECT_FALSE(axk::app::apply_filesystem_edits(read_sessions, journals, read_image->image_id, "reader",
                                                  read_image->revision, axk::PartitionIndex{0}, edits()));
    ASSERT_TRUE(read_sessions.close(read_image->image_id, "reader"));
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    const auto second = sessions.open({"workspace", "image.hds"}, "other");
    ASSERT_TRUE(opened);
    ASSERT_TRUE(second);
    EXPECT_FALSE(axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                                  axk::PartitionIndex{0}, edits()));
    EXPECT_EQ(digest(), before);
    ASSERT_TRUE(sessions.close(second->image_id, "other"));
    EXPECT_TRUE(axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                                 axk::PartitionIndex{0}, edits()));
}

TEST_F(FilesystemEditOperations, CancelledCommitLeavesTheSameRevisionReadableAndRetryable) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto before = digest();
    axk::CancellationSource cancellation;
    axk::app::AlterationJournalStore journals{root / "journals", 32U * 1024U * 1024U,
                                              [&](std::string_view phase, std::size_t index) {
                                                  if (phase == "after-patch-chunk" && index == 0U)
                                                      cancellation.cancel();
                                                  return false;
                                              }};
    const auto cancelled =
        axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                         axk::PartitionIndex{0}, edits(), cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, "operation_cancelled");
    EXPECT_EQ(digest(), before);
    {
        const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
        ASSERT_TRUE(read) << read.error().message;
    }
    const auto retry = axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                                        axk::PartitionIndex{0}, edits());
    EXPECT_TRUE(retry) << retry.error().message;
}

TEST_F(FilesystemEditOperations, ChangedImportInputRollsBackWithoutChangingSessionIdentity) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto before = digest();
    auto input = std::make_shared<MutableFileInput>();
    const std::vector<axk::FilesystemEdit> changes{axk::PutFilesystemFile{{"file.bin"}, input}};
    axk::app::AlterationJournalStore journals{root / "journals", 32U * 1024U * 1024U,
                                              [&](std::string_view phase, std::size_t) {
                                                  if (phase == "after-patch")
                                                      input->value = std::byte{0x55};
                                                  return false;
                                              }};
    const auto failed = axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner",
                                                         opened->revision, axk::PartitionIndex{0}, changes);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, "filesystem_input_changed");
    EXPECT_EQ(digest(), before);
    {
        const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
        ASSERT_TRUE(read) << read.error().message;
    }
    const auto retry = axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", opened->revision,
                                                        axk::PartitionIndex{0}, changes);
    EXPECT_TRUE(retry) << retry.error().message;
}

TEST_F(FilesystemEditOperations, RejectsAnExternallyChangedSourceBeforePlanning) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto path = root / "workspace/image.hds";
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds{1});
    const auto before = digest();
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto rejected = axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner",
                                                           opened->revision, axk::PartitionIndex{0}, edits());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "image_source_changed");
    EXPECT_EQ(digest(), before);
}

TEST_F(FilesystemEditOperations, RegisteredJobImportsExactHostAndUploadBytesAndDeletesWithConfirmation) {
    using Json = nlohmann::json;
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    axk::app::AlterationJournalStore journals{root / "journals"};
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, sessions, journals));
    const auto roots = sessions.filesystem(opened->image_id, "owner", opened->revision);
    ASSERT_TRUE(roots);
    const auto root_id = roots->items.front().id;
    {
        std::ofstream file{root / "workspace/input.bin", std::ios::binary};
        file << "Exact raw bytes";
        ASSERT_TRUE(file);
    }
    const auto upload =
        uploads.create({"owner", "empty.bin", axk::app::UploadKind::file, "application/octet-stream", 0U, {}});
    ASSERT_TRUE(upload);
    ASSERT_TRUE(uploads.complete(upload->reference, "owner"));
    Json request{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"acknowledgeDeviceRelationships", true},
        {"edits", Json::array({{{"kind", "CREATE_DIRECTORY"}, {"parentEntryId", root_id}, {"relativePath", {"Docs"}}},
                               {{"kind", "PUT_FILE"},
                                {"parentEntryId", root_id},
                                {"relativePath", {"Docs", "raw.bin"}},
                                {"source", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "input.bin"}}}}}},
                               {{"kind", "PUT_FILE"},
                                {"parentEntryId", root_id},
                                {"relativePath", {"Docs", "empty.bin"}},
                                {"source", {{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}}})}};
    const axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto accesses = registry.path_accesses("images.filesystem.edit", request, context);
    ASSERT_TRUE(accesses);
    ASSERT_EQ(accesses->size(), 1U);
    EXPECT_EQ(accesses->front().reference.relative_path, "input.bin");
    EXPECT_EQ(accesses->front().mode, axk::app::PathAccessMode::shared);
    const auto inspection = registry.invoke(
        "filesystem.inputs.inspect",
        {{"inputs", Json::array({request["edits"][1]["source"], request["edits"][2]["source"]})}}, context);
    ASSERT_TRUE(inspection) << inspection.error().message;
    request["edits"][1]["expectedSource"] = inspection->at("inputs")[0].at("snapshot");
    request["edits"][2]["expectedSource"] = inspection->at("inputs")[1].at("snapshot");
    auto conflicting = request;
    auto duplicate = request["edits"][1];
    duplicate["relativePath"] = {"Docs", "copy.bin"};
    duplicate["expectedSource"]["sha256"] = "conflicting-review";
    conflicting["edits"].push_back(duplicate);
    const auto rejected_duplicate = registry.invoke("images.filesystem.edit", conflicting, context);
    ASSERT_FALSE(rejected_duplicate);
    EXPECT_EQ(rejected_duplicate.error().code, "filesystem_input_changed");
    duplicate["expectedSource"] = request["edits"][1]["expectedSource"];
    request["edits"].push_back(duplicate);
    const auto committed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_TRUE(committed) << committed.error().message;
    EXPECT_EQ(committed->at("warnings"), nlohmann::json::array());
    EXPECT_EQ(committed->at("revision"), 2U);
    const auto children = sessions.filesystem(opened->image_id, "owner", 2U, {.parent_id = root_id});
    ASSERT_TRUE(children);
    const auto docs = std::ranges::find(children->items, "Docs", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(docs, children->items.end());
    const auto files = sessions.filesystem(opened->image_id, "owner", 2U, {.parent_id = docs->id});
    ASSERT_TRUE(files);
    ASSERT_EQ(files->items.size(), 3U);
    EXPECT_EQ(files->items[0].size_bytes, 15U);
    EXPECT_EQ(files->items[1].size_bytes, 0U);
    {
        const auto image = axk::open_image(root / "workspace/image.hds");
        ASSERT_TRUE(image);
        const auto &partition = image->partitions().front();
        const auto record = std::ranges::find(partition.records, 15U, &axk::IndexRecord::data_size);
        ASSERT_NE(record, partition.records.end());
        const auto bytes = image->read_record_data(partition.index, record->sfs_id, 15U);
        ASSERT_TRUE(bytes);
        EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size()), "Exact raw bytes");
    }
    const auto before = digest();
    EXPECT_FALSE(registry.invoke("images.filesystem.edit", request, context));
    request["expectedRevision"] = 2U;
    request["edits"] = Json::array({{{"kind", "DELETE"}, {"entryId", docs->id}, {"recursive", false}}});
    EXPECT_FALSE(registry.invoke("images.filesystem.edit", request, context));
    EXPECT_EQ(digest(), before);
    request["edits"][0]["recursive"] = true;
    request["acknowledgeDeviceRelationships"] = false;
    EXPECT_FALSE(registry.invoke("images.filesystem.edit", request, context));
    EXPECT_EQ(digest(), before);
    request["acknowledgeDeviceRelationships"] = true;
    const auto removed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_EQ(removed->at("revision"), 3U);
    const auto remaining = sessions.filesystem(opened->image_id, "owner", 3U, {.root_id = root_id, .query = "Docs"});
    ASSERT_TRUE(remaining);
    EXPECT_TRUE(remaining->items.empty());
}

TEST_F(FilesystemEditOperations, FilesEntryResolutionKeepsPartitionsSeparateEvenWithMatchingNames) {
    const axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {{"Same", {}}, {"Same", {}}}};
    ASSERT_TRUE(axk::write_hds_image(manifest, root / "workspace/two.hds"));
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({"workspace", "two.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto roots = sessions.filesystem(opened->image_id, "owner", 1U);
    ASSERT_TRUE(roots);
    ASSERT_EQ(roots->items.size(), 2U);
    std::vector<axk::app::ImageFilesystemEdit> changes;
    for (std::size_t i = 0; i < roots->items.size(); ++i) {
        const axk::app::ImageFilesystemEdit change =
            axk::app::CreateImageFilesystemDirectory{roots->items[i].id, {"New"}};
        changes.push_back(change);
        const std::vector<axk::app::ImageFilesystemEdit> single{change};
        const auto resolved = sessions.resolve_filesystem_edits(opened->image_id, "owner", 1U, single);
        ASSERT_TRUE(resolved) << resolved.error().message;
        EXPECT_EQ(resolved->partition.value, static_cast<std::uint8_t>(i));
    }
    const auto mixed = sessions.resolve_filesystem_edits(opened->image_id, "owner", 1U, changes);
    ASSERT_FALSE(mixed);
    EXPECT_EQ(mixed.error().code, "invalid_request");
}

TEST_F(FilesystemEditOperations, InspectsRawInputsAndRejectsChangesSinceReview) {
    using Json = nlohmann::json;
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto roots = sessions.filesystem(opened->image_id, "owner", opened->revision);
    ASSERT_TRUE(roots);
    axk::app::AlterationJournalStore journals{root / "journals"};
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, sessions, journals));
    const axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "review", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto path = root / "workspace/input.bin";
    {
        std::ofstream file{path, std::ios::binary};
        file << "old";
    }
    const Json source{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "input.bin"}}}};
    const Json inspection_request{{"inputs", Json::array({source})}};
    const auto inspection = registry.invoke("filesystem.inputs.inspect", inspection_request, context);
    ASSERT_TRUE(inspection) << inspection.error().message;
    ASSERT_EQ(inspection->at("inputs").size(), 1U);
    const auto snapshot = inspection->at("inputs")[0].at("snapshot");
    EXPECT_EQ(snapshot.at("sizeBytes"), 3U);
    EXPECT_EQ(snapshot.at("sha256"), axk::app::detail::file_sha256(path).value());
    EXPECT_FALSE(snapshot.at("revision").get<std::string>().empty());
    EXPECT_EQ(inspection->at("inputs")[0].at("source"), source);
    Json request{{"imageId", opened->image_id},
                 {"expectedRevision", opened->revision},
                 {"acknowledgeDeviceRelationships", true},
                 {"edits", Json::array({{{"kind", "PUT_FILE"},
                                         {"parentEntryId", roots->items.front().id},
                                         {"relativePath", {"raw.bin"}},
                                         {"source", source},
                                         {"expectedSource", snapshot}}})}};
    const auto before = digest();
    const auto timestamp = std::filesystem::last_write_time(path);
    {
        std::ofstream file{path, std::ios::binary};
        file << "new";
    }
    std::filesystem::last_write_time(path, timestamp);
    const auto changed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, "filesystem_input_changed");
    EXPECT_EQ(digest(), before);
    const auto updated = registry.invoke("filesystem.inputs.inspect", inspection_request, context);
    ASSERT_TRUE(updated);
    request["edits"][0]["expectedSource"] = updated->at("inputs")[0].at("snapshot");
    const auto committed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_TRUE(committed) << committed.error().message;
    EXPECT_EQ(committed->at("revision"), 2U);
}

TEST_F(FilesystemEditOperations, RejectsMissingSnapshotAndSameContentReplacementWithoutMutation) {
    using Json = nlohmann::json;
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto roots = sessions.filesystem(opened->image_id, "owner", opened->revision);
    ASSERT_TRUE(roots);
    axk::app::AlterationJournalStore journals{root / "journals"};
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, sessions, journals));
    const axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "review", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto path = root / "workspace/input.bin";
    {
        std::ofstream file{path, std::ios::binary};
        file << "same";
    }
    const Json source{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "input.bin"}}}};
    const auto inspection = registry.invoke("filesystem.inputs.inspect", {{"inputs", Json::array({source})}}, context);
    ASSERT_TRUE(inspection);
    Json request{{"imageId", opened->image_id},
                 {"expectedRevision", opened->revision},
                 {"acknowledgeDeviceRelationships", true},
                 {"edits", Json::array({{{"kind", "PUT_FILE"},
                                         {"parentEntryId", roots->items.front().id},
                                         {"relativePath", {"raw.bin"}},
                                         {"source", source}}})}};
    const auto before = digest();
    const auto missing = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "invalid_request");
    const auto timestamp = std::filesystem::last_write_time(path);
    std::filesystem::rename(path, root / "workspace/original.bin");
    {
        std::ofstream file{path, std::ios::binary};
        file << "same";
    }
    std::filesystem::last_write_time(path, timestamp);
    request["edits"][0]["expectedSource"] = inspection->at("inputs")[0].at("snapshot");
    const auto changed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, "filesystem_input_changed");
    EXPECT_EQ(digest(), before);
}

TEST_F(FilesystemEditOperations, RechecksReviewedHostIdentityDuringCommitAndRollsBackExactly) {
    using Json = nlohmann::json;
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto roots = sessions.filesystem(opened->image_id, "owner", opened->revision);
    ASSERT_TRUE(roots);
    const auto path = root / "workspace/input.bin";
    {
        std::ofstream file{path, std::ios::binary};
        file << "same";
    }
    bool replaced{};
    axk::app::AlterationJournalStore journals{root / "journals", 32U * 1024U * 1024U,
                                              [&](std::string_view phase, std::size_t) {
                                                  if (phase == "after-patch" && !replaced) {
                                                      std::filesystem::rename(path, root / "workspace/original.bin");
                                                      {
                                                          std::ofstream file{path, std::ios::binary};
                                                          file << "same";
                                                      }
                                                      replaced = true;
                                                  }
                                                  return false;
                                              }};
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, sessions, journals));
    const axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "review", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const Json source{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "input.bin"}}}};
    const auto inspection = registry.invoke("filesystem.inputs.inspect", {{"inputs", Json::array({source})}}, context);
    ASSERT_TRUE(inspection);
    Json request{{"imageId", opened->image_id},
                 {"expectedRevision", opened->revision},
                 {"acknowledgeDeviceRelationships", true},
                 {"edits", Json::array({{{"kind", "PUT_FILE"},
                                         {"parentEntryId", roots->items.front().id},
                                         {"relativePath", {"raw.bin"}},
                                         {"source", source},
                                         {"expectedSource", inspection->at("inputs")[0].at("snapshot")}}})}};
    const auto before = digest();
    const auto changed = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_FALSE(changed);
    EXPECT_TRUE(replaced);
    EXPECT_EQ(changed.error().code, "filesystem_input_changed");
    EXPECT_EQ(digest(), before);
    {
        const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
        ASSERT_TRUE(read) << read.error().message;
    }
    const auto updated = registry.invoke("filesystem.inputs.inspect", {{"inputs", Json::array({source})}}, context);
    ASSERT_TRUE(updated);
    request["edits"][0]["expectedSource"] = updated->at("inputs")[0].at("snapshot");
    const auto retry = registry.invoke("images.filesystem.edit", request, context);
    ASSERT_TRUE(retry) << retry.error().message;
    EXPECT_EQ(retry->at("revision"), 2U);
}

TEST_F(FilesystemEditOperations, InputInspectionChecksUploadOwnershipCancellationAndSourceLocks) {
    using Json = nlohmann::json;
    axk::app::ImageSessionManager sessions{*sandbox};
    axk::app::AlterationJournalStore journals{root / "journals"};
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, sessions, journals));
    const auto upload =
        uploads.create({"owner", "empty", axk::app::UploadKind::file, "application/octet-stream", 0U, {}});
    ASSERT_TRUE(upload);
    ASSERT_TRUE(uploads.complete(upload->reference, "owner"));
    const Json request{{"inputs", Json::array({{{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}})}};
    axk::app::OperationContext context{
        .owner_id = "other", .request_id = "review", .cancellation = {}, .progress = nullptr, .display_path = {}};
    EXPECT_FALSE(registry.invoke("filesystem.inputs.inspect", request, context));
    context.owner_id = "owner";
    const auto inspection = registry.invoke("filesystem.inputs.inspect", request, context);
    ASSERT_TRUE(inspection);
    EXPECT_EQ(inspection->at("inputs")[0].at("snapshot").at("sha256"),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    axk::CancellationSource cancellation;
    cancellation.cancel();
    context.cancellation = cancellation.token();
    const auto cancelled = registry.invoke("filesystem.inputs.inspect", request, context);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, "operation_cancelled");
    EXPECT_FALSE(registry.invoke("filesystem.inputs.inspect", {{"inputs", Json::array()}}, context));
    const auto accesses = registry.path_accesses(
        "filesystem.inputs.inspect",
        {{"inputs", Json::array({{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "input.bin"}}}}})}}, context);
    ASSERT_TRUE(accesses);
    ASSERT_EQ(accesses->size(), 1U);
    EXPECT_EQ(accesses->front().reference.relative_path, "input.bin");
    EXPECT_EQ(accesses->front().mode, axk::app::PathAccessMode::shared);
}

TEST_F(FilesystemEditOperations, ImportInspectionUsesTheReviewedDirectoryAndRevisionWithoutMutation) {
    using Json = nlohmann::json;
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    axk::app::AlterationJournalStore journals{root / "journals"};
    const auto initial = axk::app::apply_filesystem_edits(sessions, journals, opened->image_id, "owner", 1U,
                                                          axk::PartitionIndex{0}, edits());
    ASSERT_TRUE(initial);
    const auto roots = sessions.filesystem(opened->image_id, "owner", 2U);
    ASSERT_TRUE(roots);
    const auto children = sessions.filesystem(opened->image_id, "owner", 2U, {.parent_id = roots->items.front().id});
    ASSERT_TRUE(children);
    const auto directory = std::ranges::find(children->items, "Documents", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(directory, children->items.end());
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_filesystem_edit_operations(registry, *sandbox, uploads, sessions, journals));
    axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "review", .cancellation = {}, .progress = nullptr, .display_path = {}};
    Json request{
        {"imageId", opened->image_id},
        {"expectedRevision", 2U},
        {"parentEntryId", directory->id},
        {"entries", Json::array({{{"relativePath", {"file.bin"}}, {"directory", false}, {"sizeBytes", 0U}},
                                 {{"relativePath", {"Empty"}}, {"directory", true}, {"sizeBytes", 0U}},
                                 {{"relativePath", {"Empty", "new"}}, {"directory", false}, {"sizeBytes", 7U}}})}};
    const auto before = digest();
    const auto inspected = registry.invoke("images.filesystem.import.inspect", request, context);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->at("revision"), 2U);
    EXPECT_EQ(inspected->at("parentEntryId"), directory->id);
    EXPECT_EQ(inspected->at("conflictCount"), 0U);
    EXPECT_EQ(inspected->at("entries")[0].at("action"), "SKIP_FILE");
    EXPECT_EQ(inspected->at("entries")[0].at("existingSizeBytes"), 10001U);
    EXPECT_EQ(inspected->at("entries")[1].at("action"), "CREATE_DIRECTORY");
    EXPECT_TRUE(inspected->at("entries")[1].at("existingSizeBytes").is_null());
    EXPECT_EQ(inspected->at("entries")[2].at("action"), "CREATE_FILE");
    request["entries"][0]["directory"] = true;
    const auto conflict = registry.invoke("images.filesystem.import.inspect", request, context);
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict->at("conflictCount"), 1U);
    EXPECT_EQ(conflict->at("entries")[0].at("action"), "CONFLICT");
    EXPECT_FALSE(conflict->at("entries")[0].at("issue").get<std::string>().empty());
    request["expectedRevision"] = 1U;
    const auto stale = registry.invoke("images.filesystem.import.inspect", request, context);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "image_revision_stale");
    request["expectedRevision"] = 2U;
    context.owner_id = "other";
    EXPECT_FALSE(registry.invoke("images.filesystem.import.inspect", request, context));
    context.owner_id = "owner";
    axk::CancellationSource cancellation;
    cancellation.cancel();
    context.cancellation = cancellation.token();
    const auto cancelled = registry.invoke("images.filesystem.import.inspect", request, context);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, "operation_cancelled");
    EXPECT_EQ(digest(), before);
    EXPECT_EQ(sessions.inspect(opened->image_id, "owner")->revision, 2U);
}
} // namespace
