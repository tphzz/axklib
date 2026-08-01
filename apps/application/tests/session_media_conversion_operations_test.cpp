#include <chrono>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/session_media_conversion_operations.hpp"
#include "axklib/media.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

class SessionMediaConversionOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-session-media-conversion-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
        std::filesystem::create_directory(root_ / "exports");
        std::filesystem::copy_file(fixture_path(), root_ / "source.hds");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox) << sandbox.error().message;
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        reservations_ = std::make_unique<axk::app::PathReservationCoordinator>();
        images_ = std::make_unique<axk::app::ImageSessionManager>(*sandbox_, 4U, 500U, std::chrono::minutes{15},
                                                                  std::chrono::steady_clock::now, reservations_.get());
        downloads_ = std::make_unique<axk::app::DownloadArchiveStore>(root_ / "downloads", 32U * 1024U * 1024U,
                                                                      16U * 1024U * 1024U, 8U, std::chrono::minutes{5});
        registry_ = axk::app::make_operation_registry();
        ASSERT_TRUE(axk::app::bind_session_media_conversion_operations(registry_, *sandbox_, *images_, *downloads_));
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

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    std::unique_ptr<axk::app::PathReservationCoordinator> reservations_;
    std::unique_ptr<axk::app::ImageSessionManager> images_;
    std::unique_ptr<axk::app::DownloadArchiveStore> downloads_;
    axk::app::OperationRegistry registry_;
};

TEST_F(SessionMediaConversionOperationsTest, InspectsAndExportsPartitionAndVolumeThroughTheSamePlan) {
    const auto opened = images_->open({"workspace", "source.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = images_->content(opened->image_id, "owner", 32U);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto partition = std::ranges::find(roots->items, "partition", &axk::app::ImageContentItem::kind);
    ASSERT_NE(partition, roots->items.end());
    ASSERT_TRUE(partition->partition_index);
    const auto children = images_->content(opened->image_id, "owner", 32U, {}, partition->id);
    ASSERT_TRUE(children) << children.error().message;
    const auto volume = std::ranges::find(children->items, "volume", &axk::app::ImageContentItem::kind);
    ASSERT_NE(volume, children->items.end());
    ASSERT_TRUE(volume->volume_directory_id);

    const nlohmann::json iso_base{{"imageId", opened->image_id},
                                  {"expectedRevision", opened->revision},
                                  {"format", "ISO9660"},
                                  {"partitionIndex", *partition->partition_index}};
    const auto iso_inspection = registry_.invoke("images.media_conversion.inspect", iso_base, context());
    ASSERT_TRUE(iso_inspection) << iso_inspection.error().message;
    EXPECT_TRUE(iso_inspection->at("canExport"));
    EXPECT_EQ(iso_inspection->at("scope"), "PARTITION");
    EXPECT_EQ(iso_inspection->at("volumes").size(), 1U);
    EXPECT_EQ(iso_inspection->at("defaultFilename"), "source_p00_New_Partition.iso");

    auto iso_request = iso_base;
    iso_request["destination"] = {{"kind", "WORKSPACE"},
                                  {"output", {{"rootId", "workspace"}, {"relativePath", "exports/disk"}}},
                                  {"overwrite", false}};
    const auto iso_result = registry_.invoke("images.media_conversion", iso_request, context());
    ASSERT_TRUE(iso_result) << iso_result.error().message;
    EXPECT_EQ(iso_result->at("output").at("relativePath"), "exports/disk.iso");
    const auto iso = axk::open_media(root_ / "exports/disk.iso");
    ASSERT_TRUE(iso) << iso.error().message;
    EXPECT_EQ(iso->kind(), axk::MediaKind::iso9660);

    const nlohmann::json floppy_base{{"imageId", opened->image_id},
                                     {"expectedRevision", opened->revision},
                                     {"format", "FAT12_FLOPPY"},
                                     {"partitionIndex", *partition->partition_index},
                                     {"volumeDirectoryId", *volume->volume_directory_id}};
    const auto floppy_inspection = registry_.invoke("images.media_conversion.inspect", floppy_base, context());
    ASSERT_TRUE(floppy_inspection) << floppy_inspection.error().message;
    EXPECT_TRUE(floppy_inspection->at("canExport"));
    EXPECT_EQ(floppy_inspection->at("scope"), "VOLUME");
    EXPECT_EQ(floppy_inspection->at("projectedOutputBytes"), 1'474'560U);

    auto floppy_request = floppy_base;
    floppy_request["destination"] = {{"kind", "DOWNLOAD"}, {"filename", "Local disk"}};
    const auto floppy_result = registry_.invoke("images.media_conversion", floppy_request, context());
    ASSERT_TRUE(floppy_result) << floppy_result.error().message;
    ASSERT_EQ(floppy_result->at("download").at("filename"), "Local disk.ima");
    const auto retained = downloads_->open({floppy_result->at("download").at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(retained) << retained.error().message;
    EXPECT_EQ(retained->snapshot.size_bytes, 1'474'560U);
    const auto floppy = axk::open_media(retained->reader, "Local disk.ima");
    ASSERT_TRUE(floppy) << floppy.error().message;
    EXPECT_EQ(floppy->kind(), axk::MediaKind::fat12_floppy);
}

} // namespace
