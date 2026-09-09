#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/filesystem_export.hpp"
#include "axklib/application/filesystem_export_operations.hpp"
#include "axklib/filesystem_edit.hpp"
#include "axklib/writer.hpp"
#include "content_digest.hpp"

namespace {
class ExportProgress final : public axk::ProgressSink {
  public:
    std::function<void(const axk::Progress &)> action;
    void report(const axk::Progress &progress) noexcept override { action(progress); }
};
class FilesystemExport : public testing::Test {
  protected:
    std::filesystem::path root;
    std::unique_ptr<axk::app::Sandbox> sandbox;
    std::unique_ptr<axk::app::ImageSessionManager> sessions;
    axk::app::PathReservationCoordinator reservations;
    std::string image_id;
    std::string root_id;

    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               std::format("axk-files-export-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        const axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {{"Files", {}}}};
        ASSERT_TRUE(axk::write_hds_image(manifest, root / "blank.hds"));
        const std::vector<axk::FilesystemEdit> edits{
            axk::CreateFilesystemDirectory{{"Documents"}}, axk::CreateFilesystemDirectory{{"Documents", "Empty"}},
            axk::PutFilesystemFile{
                {"Documents", "data.bin"},
                std::make_shared<axk::MemoryReader>(std::vector<std::byte>(10001U, std::byte{0x57}))},
            axk::PutFilesystemFile{{"empty.bin"}, std::make_shared<axk::MemoryReader>(std::vector<std::byte>{})}};
        const auto written =
            axk::write_sfs_file_edits(root / "blank.hds", root / "image.hds", axk::PartitionIndex{0}, edits);
        ASSERT_TRUE(written) << written.error().message;
        auto created = axk::app::Sandbox::create({{"workspace", "Workspace", root, true}});
        ASSERT_TRUE(created);
        sandbox = std::make_unique<axk::app::Sandbox>(std::move(*created));
        sessions = std::make_unique<axk::app::ImageSessionManager>(*sandbox, 32U, 500U, std::chrono::minutes{15},
                                                                   std::chrono::steady_clock::now, &reservations);
        const auto opened = sessions->open({"workspace", "image.hds"}, "owner");
        ASSERT_TRUE(opened) << opened.error().message;
        image_id = opened->image_id;
        const auto roots = sessions->filesystem(image_id, "owner", 1U);
        ASSERT_TRUE(roots);
        root_id = roots->items.front().id;
    }
    void TearDown() override {
        sessions.reset();
        sandbox.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::vector<std::string> selection() const { return {root_id}; }
};

TEST_F(FilesystemExport, CopiesRawFilesAndEmptyDirectoriesWithoutChangingSource) {
    const auto before = axk::app::detail::file_sha256(root / "image.hds");
    ASSERT_TRUE(before);
    const auto plan = axk::app::inspect_filesystem_export(*sessions, image_id, "owner", 1U, selection());
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->total_bytes, 10001U);
    EXPECT_EQ(plan->notices.size(), 2U);
    const auto result = axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, image_id, "owner", 1U,
                                                            selection(), {"workspace", "exported"});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(std::filesystem::is_directory(root / "exported/Files/Documents/Empty"));
    EXPECT_EQ(std::filesystem::file_size(root / "exported/Files/empty.bin"), 0U);
    const auto file = axk::FileReader::open(root / "exported/Files/Documents/data.bin");
    ASSERT_TRUE(file);
    std::vector<std::byte> bytes(10001U);
    ASSERT_TRUE((*file)->read_exact_at(0U, bytes));
    EXPECT_EQ(bytes, std::vector<std::byte>(10001U, std::byte{0x57}));
    const auto after = axk::app::detail::file_sha256(root / "image.hds");
    ASSERT_TRUE(after);
    EXPECT_EQ(*after, *before);
}

TEST_F(FilesystemExport, RejectsStaleProtectedAndConflictingDestinationsWithoutPublishing) {
    EXPECT_FALSE(axk::app::inspect_filesystem_export(*sessions, image_id, "other", 1U, selection()));
    EXPECT_FALSE(axk::app::inspect_filesystem_export(*sessions, image_id, "owner", 2U, selection()));
    const auto entries = sessions->filesystem(image_id, "owner", 1U, {.parent_id = root_id});
    ASSERT_TRUE(entries);
    const auto metadata =
        std::ranges::find_if(entries->items, [](const auto &entry) { return entry.filesystem_metadata; });
    ASSERT_NE(metadata, entries->items.end());
    const std::vector<std::string> selected{metadata->id};
    EXPECT_FALSE(axk::app::inspect_filesystem_export(*sessions, image_id, "owner", 1U, selected));
    std::filesystem::create_directories(root / "existing");
    {
        std::ofstream marker{root / "existing/keep"};
        marker << "keep";
    }
    EXPECT_FALSE(axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, image_id, "owner", 1U,
                                                     selection(), {"workspace", "existing"}));
    EXPECT_TRUE(std::filesystem::exists(root / "existing/keep"));
    EXPECT_FALSE(std::filesystem::exists(root / "existing/Files"));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, image_id, "owner", 1U,
                                                     selection(), {"workspace", "cancelled"}, cancellation.token()));
    EXPECT_FALSE(std::filesystem::exists(root / "cancelled"));
}

TEST_F(FilesystemExport, NormalizesOverlappingSelectionsAndReservesTheDestination) {
    const auto all = sessions->filesystem(image_id, "owner", 1U, {.root_id = root_id});
    ASSERT_TRUE(all);
    const auto directory = std::ranges::find(all->items, "Documents", &axk::app::ImageFilesystemEntry::name);
    const auto child = std::ranges::find(all->items, "data.bin", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(directory, all->items.end());
    ASSERT_NE(child, all->items.end());
    const std::vector<std::string> selected{directory->id, child->id, directory->id};
    const auto plan = axk::app::inspect_filesystem_export(*sessions, image_id, "owner", 1U, selected);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->entries.size(), 3U);
    EXPECT_EQ(plan->total_bytes, 10001U);
    EXPECT_EQ(plan->entries.front().relative_path, (std::vector<std::string>{"Documents"}));
    auto lease = reservations.try_acquire({{"workspace", "reserved"}, axk::app::PathAccessMode::exclusive});
    ASSERT_TRUE(lease);
    const auto blocked = axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, image_id, "owner", 1U,
                                                             selected, {"workspace", "reserved"});
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, "entry_in_use");
    EXPECT_FALSE(std::filesystem::exists(root / "reserved"));
}

TEST_F(FilesystemExport, CancelsDuringStreamingOrBeforePublicationWithoutPartialOutput) {
    for (const auto phase : {axk::ProgressPhase::exporting, axk::ProgressPhase::publishing}) {
        axk::CancellationSource cancellation;
        ExportProgress progress;
        progress.action = [&](const auto &value) {
            if (value.phase == phase)
                cancellation.cancel();
        };
        const auto result =
            axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, image_id, "owner", 1U, selection(),
                                                {"workspace", "cancelled"}, cancellation.token(), &progress);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "operation_cancelled");
        EXPECT_FALSE(std::filesystem::exists(root / "cancelled"));
        EXPECT_TRUE(sessions->begin_read(image_id, "owner", 1U));
    }
}

TEST_F(FilesystemExport, RefusesToPublishAfterAnExternalSourceChange) {
    bool changed{};
    ExportProgress progress;
    progress.action = [&](const auto &value) {
        if (changed || value.phase != axk::ProgressPhase::exporting)
            return;
        std::fstream output{root / "image.hds", std::ios::binary | std::ios::in | std::ios::out};
        output.seekp(-1, std::ios::end);
        output.put('\x33');
        output.close();
        changed = !!output;
    };
    const auto result = axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, image_id, "owner", 1U,
                                                            selection(), {"workspace", "changed"}, {}, &progress);
    EXPECT_TRUE(changed);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "image_source_changed");
    EXPECT_FALSE(std::filesystem::exists(root / "changed"));
}

TEST_F(FilesystemExport, ReviewsHostNamesAndRejectsCollisionsBeforePublication) {
    const auto empty = std::make_shared<axk::MemoryReader>(std::vector<std::byte>{});
    const std::vector<axk::FilesystemEdit> edits{axk::PutFilesystemFile{{"CON.txt"}, empty},
                                                 axk::PutFilesystemFile{{"a:b"}, empty},
                                                 axk::PutFilesystemFile{{"a?b"}, empty}};
    const auto written =
        axk::write_sfs_file_edits(root / "blank.hds", root / "names.hds", axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(written) << written.error().message;
    const auto opened = sessions->open({"workspace", "names.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto roots = sessions->filesystem(opened->image_id, "owner", 1U);
    ASSERT_TRUE(roots);
    const auto entries = sessions->filesystem(opened->image_id, "owner", 1U, {.parent_id = roots->items.front().id});
    ASSERT_TRUE(entries);
    const auto reserved = std::ranges::find(entries->items, "CON.txt", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(reserved, entries->items.end());
    const std::vector<std::string> selected{reserved->id};
    const auto plan = axk::app::inspect_filesystem_export(*sessions, opened->image_id, "owner", 1U, selected);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->entries.size(), 1U);
    EXPECT_EQ(plan->entries.front().relative_path, (std::vector<std::string>{"_CON.txt"}));
    ASSERT_EQ(plan->notices.size(), 1U);
    EXPECT_EQ(plan->notices.front().entry_id, reserved->id);
    const std::vector<std::string> all{roots->items.front().id};
    const auto collision = axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, opened->image_id,
                                                               "owner", 1U, all, {"workspace", "collision"});
    ASSERT_FALSE(collision);
    EXPECT_EQ(collision.error().code, "filesystem_export_collision");
    EXPECT_FALSE(std::filesystem::exists(root / "collision"));
}

TEST_F(FilesystemExport, JobsInspectAndExportToWorkspaceOrOwnedDownload) {
    auto registry = axk::app::make_operation_registry();
    axk::app::DownloadArchiveStore downloads{root / "downloads", 1024U * 1024U, 1024U * 1024U, 100U,
                                             std::chrono::minutes{10}};
    ASSERT_TRUE(axk::app::bind_filesystem_export_operations(registry, *sandbox, *sessions, downloads));
    const axk::app::OperationContext context{"owner", "export-test", {}, nullptr, {}};
    nlohmann::json request{
        {"imageId", image_id}, {"expectedRevision", 1U}, {"entryIds", selection()}, {"layout", "SELECTED_ENTRIES"}};
    const auto inspected = registry.invoke("images.filesystem.export.inspect", request, context);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->at("totalBytes"), 10001U);
    EXPECT_EQ(inspected->at("notices").size(), 2U);
    request["destination"] = {{"kind", "WORKSPACE"},
                              {"output", {{"rootId", "workspace"}, {"relativePath", "job-out"}}}};
    const auto accesses = registry.path_accesses("images.filesystem.export", request, context);
    ASSERT_TRUE(accesses);
    ASSERT_EQ(accesses->size(), 1U);
    EXPECT_EQ(accesses->front().reference.relative_path, "job-out");
    EXPECT_EQ(accesses->front().mode, axk::app::PathAccessMode::exclusive);
    const auto exported = registry.invoke("images.filesystem.export", request, context);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("destination"), "WORKSPACE");
    EXPECT_TRUE(exported->at("download").is_null());
    EXPECT_EQ(std::filesystem::file_size(root / "job-out/Files/Documents/data.bin"), 10001U);
    request["destination"] = {{"kind", "DOWNLOAD"}, {"directoryName", "Files"}};
    const auto downloaded = registry.invoke("images.filesystem.export", request, context);
    ASSERT_TRUE(downloaded) << downloaded.error().message;
    EXPECT_EQ(downloaded->at("destination"), "DOWNLOAD");
    EXPECT_TRUE(downloaded->at("output").is_null());
    EXPECT_EQ(downloaded->at("download").size(), 5U);
    const axk::app::DownloadArchiveRef reference{downloaded->at("download").at("archiveId").get<std::string>()};
    const auto content = downloads.open(reference, "owner");
    ASSERT_TRUE(content);
    EXPECT_EQ(content->snapshot.filename, "Files.tar");
    EXPECT_GT(content->snapshot.size_bytes, 10001U);
    EXPECT_FALSE(downloads.open(reference, "other"));
    EXPECT_TRUE(registry.path_accesses("images.filesystem.export", request, context)->empty());
    request["expectedRevision"] = 2U;
    const auto stale = registry.invoke("images.filesystem.export", request, context);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "image_revision_stale");
    request["expectedRevision"] = 1U;
    request["destination"]["directoryName"] = "../escape";
    EXPECT_FALSE(registry.invoke("images.filesystem.export", request, context));
    request["destination"]["directoryName"] = "Files";
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const axk::app::OperationContext cancelled{"owner", "cancel-export", cancellation.token(), nullptr, {}};
    const auto stopped = registry.invoke("images.filesystem.export", request, cancelled);
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, "operation_cancelled");
}

TEST_F(FilesystemExport, FolderExportUsesTheChosenDestinationAsTheSelectedDirectory) {
    auto registry = axk::app::make_operation_registry();
    axk::app::DownloadArchiveStore downloads{root / "downloads", 65536U, 65536U, 100U, std::chrono::minutes{10}};
    ASSERT_TRUE(axk::app::bind_filesystem_export_operations(registry, *sandbox, *sessions, downloads));
    const auto entries = sessions->filesystem(image_id, "owner", 1U, {.root_id = root_id});
    ASSERT_TRUE(entries);
    const auto directory = std::ranges::find(entries->items, "Documents", &axk::app::ImageFilesystemEntry::name);
    ASSERT_NE(directory, entries->items.end());
    nlohmann::json request{
        {"imageId", image_id},
        {"expectedRevision", 1U},
        {"entryIds", {directory->id}},
        {"layout", "EXPORT_FOLDER"},
        {"destination", {{"kind", "WORKSPACE"}, {"output", {{"rootId", "workspace"}, {"relativePath", "Renamed"}}}}}};
    const auto result =
        registry.invoke("images.filesystem.export", request, {"owner", "folder-export", {}, nullptr, {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(std::filesystem::is_directory(root / "Renamed/Empty"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "Renamed/data.bin"));
    EXPECT_FALSE(std::filesystem::exists(root / "Renamed/Documents"));
    EXPECT_EQ(result->at("rootDirectory").at("name"), "Documents");
    EXPECT_EQ(result->at("entries").size(), 2U);
    auto inspection = request;
    inspection.erase("destination");
    EXPECT_TRUE(registry.invoke("images.filesystem.export.inspect", inspection, {"owner", "inspect", {}, nullptr, {}}));
    inspection.erase("layout");
    EXPECT_FALSE(
        registry.invoke("images.filesystem.export.inspect", inspection, {"owner", "inspect", {}, nullptr, {}}));
    inspection["layout"] = "INVALID";
    EXPECT_FALSE(
        registry.invoke("images.filesystem.export.inspect", inspection, {"owner", "inspect", {}, nullptr, {}}));
}

TEST_F(FilesystemExport, FolderLayoutHandlesEmptySingleMultipleAndOverlappingSelections) {
    const auto before = axk::app::detail::file_sha256(root / "image.hds");
    const auto entries = sessions->filesystem(image_id, "owner", 1U, {.root_id = root_id});
    ASSERT_TRUE(entries);
    const auto id = [&](std::string_view name) {
        return std::ranges::find(entries->items, name, &axk::app::ImageFilesystemEntry::name)->id;
    };
    const auto run = [&](std::vector<std::string> selected, const std::string &name) {
        return axk::app::export_filesystem_entries(*sessions, *sandbox, reservations, image_id, "owner", 1U, selected,
                                                   {"workspace", name}, {}, nullptr,
                                                   axk::app::FilesystemExportLayout::export_folder);
    };
    const auto empty = run({id("Empty")}, "empty-out");
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->root_directory.has_value());
    EXPECT_TRUE(empty->entries.empty());
    EXPECT_TRUE(std::filesystem::is_empty(root / "empty-out"));
    const auto single = run({id("data.bin")}, "single-out");
    ASSERT_TRUE(single);
    EXPECT_FALSE(single->root_directory.has_value());
    EXPECT_EQ(std::filesystem::file_size(root / "single-out/data.bin"), 10001U);
    const auto multiple = run({id("Documents"), id("empty.bin")}, "multi-out");
    ASSERT_TRUE(multiple);
    EXPECT_FALSE(multiple->root_directory.has_value());
    EXPECT_TRUE(std::filesystem::exists(root / "multi-out/Documents/data.bin"));
    EXPECT_TRUE(std::filesystem::exists(root / "multi-out/empty.bin"));
    const auto overlapping = run({id("Documents"), id("data.bin"), id("Documents")}, "overlap-out");
    ASSERT_TRUE(overlapping);
    EXPECT_EQ(overlapping->entries.size(), 2U);
    EXPECT_TRUE(std::filesystem::exists(root / "overlap-out/data.bin"));
    EXPECT_FALSE(std::filesystem::exists(root / "overlap-out/Documents"));
    const auto partition = run({root_id}, "partition-out");
    ASSERT_TRUE(partition);
    EXPECT_TRUE(std::filesystem::exists(root / "partition-out/Documents/data.bin"));
    EXPECT_FALSE(std::filesystem::exists(root / "partition-out/Files"));
    const auto after = axk::app::detail::file_sha256(root / "image.hds");
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(*after, *before);
}

TEST_F(FilesystemExport, DownloadCancellationAfterTheLastPayloadDoesNotRetainAnArchive) {
    auto registry = axk::app::make_operation_registry();
    axk::app::DownloadArchiveStore downloads{root / "downloads", 65536U, 65536U, 100U, std::chrono::minutes{10}};
    ASSERT_TRUE(axk::app::bind_filesystem_export_operations(registry, *sandbox, *sessions, downloads));
    const auto entries = sessions->filesystem(image_id, "owner", 1U, {.root_id = root_id, .query = "data.bin"});
    ASSERT_TRUE(entries);
    ASSERT_EQ(entries->items.size(), 1U);
    const nlohmann::json request{{"imageId", image_id},
                                 {"expectedRevision", 1U},
                                 {"layout", "SELECTED_ENTRIES"},
                                 {"entryIds", {entries->items.front().id}},
                                 {"destination", {{"kind", "DOWNLOAD"}, {"directoryName", "Data"}}}};
    axk::CancellationSource cancellation;
    ExportProgress progress;
    progress.action = [&](const auto &value) {
        if (value.phase == axk::ProgressPhase::writing && value.completed > 0U)
            cancellation.cancel();
    };
    const axk::app::OperationContext context{"owner", "cancel-archive", cancellation.token(), &progress, {}};
    const auto result = registry.invoke("images.filesystem.export", request, context);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "operation_cancelled");
    EXPECT_TRUE(std::filesystem::is_empty(root / "downloads"));
    EXPECT_TRUE(registry.invoke("images.filesystem.export", request, {"owner", "retry", {}, nullptr, {}}));
}
} // namespace
