#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/session_volume_floppy_export_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/writer.hpp"

namespace {

axk::VolumeSpec source_volume(const std::filesystem::path &audio_path, std::string name) {
    axk::VolumeSpec volume;
    volume.name = std::move(name);
    volume.waveforms.push_back({"wave", "Wave Data", audio_path, 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60U;
    volume.samples.push_back(std::move(sample));
    axk::SampleSpec direct_sample;
    direct_sample.name = "Direct Sample";
    direct_sample.waveform_id = "wave";
    direct_sample.root_key = 60U;
    volume.samples.push_back(std::move(direct_sample));
    volume.sample_banks.push_back({"Sample Bank", {"Sample"}});
    volume.programs.push_back({1U, {{"SBAC", "Sample Bank", 1U}, {"SBNK", "Direct Sample", 2U}}});
    return volume;
}

class SessionVolumeFloppyExportOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-session-volume-floppy-export-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);

        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = 4U;
        waveform.pcm.resize(8U);
        ASSERT_TRUE(axk::write_wav_atomic(root_ / "source.wav", waveform));
        axk::VolumeSpec empty;
        empty.name = "Empty Volume";
        const axk::HdsBuildManifest manifest{
            "1.0",
            8U * 1024U * 1024U,
            {{"PARTITION ONE",
              {source_volume(root_ / "source.wav", "First Volume"),
               source_volume(root_ / "source.wav", "Second Volume"), std::move(empty)}}}};
        const auto written = axk::write_hds_image(manifest, root_ / "source.hds");
        ASSERT_TRUE(written) << written.error().message;

        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox) << sandbox.error().message;
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        reservations_ = std::make_unique<axk::app::PathReservationCoordinator>();
        images_ = std::make_unique<axk::app::ImageSessionManager>(*sandbox_, 4U, 500U, std::chrono::minutes{15},
                                                                  std::chrono::steady_clock::now, reservations_.get());
        downloads_ = std::make_unique<axk::app::DownloadArchiveStore>(root_ / "downloads", 32U * 1024U * 1024U,
                                                                      16U * 1024U * 1024U, 8U, std::chrono::minutes{5});
        registry_ = axk::app::make_operation_registry();
        ASSERT_TRUE(
            axk::app::bind_session_volume_floppy_export_operations(registry_, *sandbox_, *images_, *downloads_));
    }

    void TearDown() override {
        images_.reset();
        downloads_.reset();
        reservations_.reset();
        sandbox_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] axk::app::OperationContext context() const {
        return {
            .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    }

    [[nodiscard]] nlohmann::json base_request() {
        const auto opened = images_->open({"workspace", "source.hds"}, "owner");
        EXPECT_TRUE(opened) << opened.error().message;
        const auto roots = images_->content(opened->image_id, "owner", 32U);
        EXPECT_TRUE(roots) << roots.error().message;
        const auto partition = std::ranges::find(roots->items, "partition", &axk::app::ImageContentItem::kind);
        EXPECT_NE(partition, roots->items.end());
        return {{"imageId", opened->image_id}, {"expectedRevision", opened->revision}, {"scopeId", partition->id}};
    }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    std::unique_ptr<axk::app::PathReservationCoordinator> reservations_;
    std::unique_ptr<axk::app::ImageSessionManager> images_;
    std::unique_ptr<axk::app::DownloadArchiveStore> downloads_;
    axk::app::OperationRegistry registry_;
};

TEST_F(SessionVolumeFloppyExportOperationsTest, InspectsAndPublishesRawVolumeFloppyDirectoriesWithReport) {
    const auto base = base_request();
    const auto inspection = registry_.invoke("images.volume_floppy_export.inspect", base, context());
    ASSERT_TRUE(inspection) << inspection.error().message;
    EXPECT_EQ(inspection->at("sourceMediaKind"), "SFS");
    EXPECT_EQ(inspection->at("volumeCount"), 3U);
    EXPECT_EQ(inspection->at("exportableCount"), 2U);
    EXPECT_EQ(inspection->at("emptyCount"), 1U);
    EXPECT_EQ(inspection->at("blockedCount"), 0U);
    EXPECT_EQ(inspection->at("totalFloppyImageCount"), 2U);

    auto request = base;
    request["destination"] = {{"kind", "WORKSPACE"},
                              {"output", {{"rootId", "workspace"}, {"relativePath", "partition-floppies"}}}};
    const auto exported = registry_.invoke("images.volume_floppy_export", request, context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("exportedCount"), 2U);
    EXPECT_EQ(exported->at("skippedCount"), 1U);
    EXPECT_EQ(exported->at("blockedCount"), 0U);
    EXPECT_EQ(exported->at("failedCount"), 0U);
    EXPECT_EQ(exported->at("floppyImageCount"), 2U);
    EXPECT_EQ(exported->at("reportPath"), "volume-floppies.axklib.json");

    const auto output = root_ / "partition-floppies";
    ASSERT_TRUE(std::filesystem::is_regular_file(output / "volume-floppies.axklib.json"));
    std::ifstream report_input{output / "volume-floppies.axklib.json"};
    const auto report = nlohmann::json::parse(report_input);
    EXPECT_EQ(report.at("summary").at("exportedCount"), 2U);
    EXPECT_EQ(report.at("summary").at("skippedCount"), 1U);
    std::set<std::string> directories;
    for (const auto &volume : exported->at("volumes")) {
        if (volume.at("status") != "EXPORTED")
            continue;
        const auto directory = volume.at("directoryPath").get<std::string>();
        directories.insert(directory);
        ASSERT_EQ(volume.at("disks").size(), 1U);
        EXPECT_EQ(volume.at("disks").front().at("path"), directory + "/disk01.ima");
        const auto disk_path = output / directory / "disk01.ima";
        ASSERT_TRUE(std::filesystem::is_regular_file(disk_path));
        EXPECT_EQ(std::filesystem::file_size(disk_path), 1'474'560U);
    }
    EXPECT_EQ(directories.size(), 2U);
    EXPECT_FALSE(std::filesystem::exists(output / "Empty Volume"));
}

TEST_F(SessionVolumeFloppyExportOperationsTest, RetainsTheDirectoryAsATarForDesktopDownload) {
    auto request = base_request();
    request["destination"] = {{"kind", "DOWNLOAD"}, {"directoryName", "local-floppies"}};
    const auto exported = registry_.invoke("images.volume_floppy_export", request, context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("destination"), "DOWNLOAD");
    EXPECT_EQ(exported->at("download").at("filename"), "local-floppies.tar");
    const auto retained = downloads_->open({exported->at("download").at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(retained) << retained.error().message;
    EXPECT_EQ(retained->snapshot.entry_count, 7U);
}

TEST_F(SessionVolumeFloppyExportOperationsTest, CancellationPublishesNothing) {
    auto request = base_request();
    request["destination"] = {{"kind", "WORKSPACE"},
                              {"output", {{"rootId", "workspace"}, {"relativePath", "cancelled"}}}};
    axk::CancellationSource cancellation;
    cancellation.cancel();
    auto operation_context = context();
    operation_context.cancellation = cancellation.token();
    const auto exported = registry_.invoke("images.volume_floppy_export", request, operation_context);
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, "operation_cancelled");
    EXPECT_FALSE(std::filesystem::exists(root_ / "cancelled"));
}

} // namespace
