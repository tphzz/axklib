#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/session_media_conversion_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"

namespace {

std::optional<std::string> environment_value(std::string_view key) {
#if defined(_WIN32)
    char *raw = nullptr;
    std::size_t size = 0U;
    if (_dupenv_s(&raw, &size, std::string{key}.c_str()) != 0 || raw == nullptr)
        return std::nullopt;
    const std::unique_ptr<char, decltype(&std::free)> value{raw, &std::free};
    return *value == '\0' ? std::nullopt : std::optional<std::string>{value.get()};
#else
    const auto *value = std::getenv(std::string{key}.c_str());
    return value == nullptr || *value == '\0' ? std::nullopt : std::optional<std::string>{value};
#endif
}

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
    EXPECT_EQ(iso_inspection->at("artifactKind"), "IMAGE");
    EXPECT_EQ(iso_inspection->at("outputExtension"), ".iso");
    EXPECT_EQ(iso_inspection->at("floppyImageCount"), 0U);
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
    EXPECT_EQ(floppy_inspection->at("artifactKind"), "IMAGE");
    EXPECT_EQ(floppy_inspection->at("outputExtension"), ".ima");
    EXPECT_EQ(floppy_inspection->at("floppyImageCount"), 1U);
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

TEST_F(SessionMediaConversionOperationsTest, ExportsAnOversizedVolumeAsATypedMultiFloppyZip) {
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 800'000U;
    waveform.pcm.resize(static_cast<std::size_t>(waveform.frame_count) * 2U);
    for (std::uint64_t frame = 0U; frame < waveform.frame_count; ++frame) {
        const auto phase = static_cast<std::int32_t>(frame % 100U);
        const auto value = static_cast<std::int16_t>(phase < 50 ? -12'000 + phase * 480 : 12'000 - (phase - 50) * 480);
        const auto bits = static_cast<std::uint16_t>(value);
        waveform.pcm[static_cast<std::size_t>(frame) * 2U] = static_cast<std::byte>(bits & 0xffU);
        waveform.pcm[static_cast<std::size_t>(frame) * 2U + 1U] = static_cast<std::byte>(bits >> 8U);
    }
    ASSERT_TRUE(axk::write_wav_atomic(root_ / "long.wav", waveform));

    axk::VolumeSpec volume;
    volume.name = "Long Volume";
    volume.waveforms.push_back({"long", "Long Wave", root_ / "long.wav", 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Bank Sample";
    sample.waveform_id = "long";
    sample.root_key = 60U;
    sample.key_high = 127U;
    volume.samples.push_back(std::move(sample));
    axk::SampleSpec direct_sample;
    direct_sample.name = "Direct Sample";
    direct_sample.waveform_id = "long";
    direct_sample.root_key = 60U;
    direct_sample.key_high = 127U;
    volume.samples.push_back(std::move(direct_sample));
    volume.sample_banks.push_back({"Long Bank", {"Bank Sample"}});
    volume.programs.push_back({1U, "Pgm 001", {{"SBAC", "Long Bank", 1U}, {"SBNK", "Direct Sample", 2U}}});
    const axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {{"PARTITION ONE", {std::move(volume)}}}};
    const auto written = axk::write_hds_image(manifest, root_ / "long.hds");
    ASSERT_TRUE(written) << written.error().message;

    const auto opened = images_->open({"workspace", "long.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = images_->content(opened->image_id, "owner", 32U);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto partition = std::ranges::find(roots->items, "partition", &axk::app::ImageContentItem::kind);
    ASSERT_NE(partition, roots->items.end());
    const auto children = images_->content(opened->image_id, "owner", 32U, {}, partition->id);
    ASSERT_TRUE(children) << children.error().message;
    const auto selected_volume = std::ranges::find(children->items, "volume", &axk::app::ImageContentItem::kind);
    ASSERT_NE(selected_volume, children->items.end());
    ASSERT_TRUE(partition->partition_index);
    ASSERT_TRUE(selected_volume->volume_directory_id);

    const nlohmann::json base{{"imageId", opened->image_id},
                              {"expectedRevision", opened->revision},
                              {"format", "FAT12_FLOPPY"},
                              {"partitionIndex", *partition->partition_index},
                              {"volumeDirectoryId", *selected_volume->volume_directory_id}};
    const auto inspection = registry_.invoke("images.media_conversion.inspect", base, context());
    ASSERT_TRUE(inspection) << inspection.error().message;
    EXPECT_TRUE(inspection->at("canExport"));
    EXPECT_EQ(inspection->at("artifactKind"), "FLOPPY_DISK_SET");
    EXPECT_EQ(inspection->at("outputExtension"), ".zip");
    EXPECT_EQ(inspection->at("floppyImageCount"), 2U);
    EXPECT_EQ(inspection->at("defaultFilename"), "long_p00_Long_Volume.zip");

    auto request = base;
    request["destination"] = {{"kind", "DOWNLOAD"}, {"filename", "Long volume"}};
    const auto result = registry_.invoke("images.media_conversion", request, context());
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->at("download").at("filename"), "Long volume.zip");
    const auto retained = downloads_->open({result->at("download").at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(retained) << retained.error().message;
    EXPECT_EQ(retained->snapshot.media_type, "application/zip");
    ASSERT_LE(retained->reader->size(), std::numeric_limits<std::size_t>::max());
    std::vector<std::byte> archive_bytes(static_cast<std::size_t>(retained->reader->size()));
    ASSERT_TRUE(retained->reader->read_exact_at(0U, archive_bytes));
    const auto archive = axk::package_internal::read_archive(archive_bytes);
    ASSERT_TRUE(archive) << archive.error().message;
    ASSERT_EQ(archive->size(), 3U);
    EXPECT_EQ(archive->front().path, "manifest.json");

    axk::MemoryReader first_disk{archive->at(1U).bytes};
    axk::MemoryReader second_disk{archive->at(2U).bytes};
    ASSERT_TRUE(sandbox_->publish_file({"workspace", "disk01.ima"}, false, first_disk));
    ASSERT_TRUE(sandbox_->publish_file({"workspace", "disk02.ima"}, false, second_disk));

    const auto final_member_opened =
        images_->open({"workspace", "disk02.ima", axk::app::ImageSourceKind::file}, "owner");
    ASSERT_TRUE(final_member_opened) << final_member_opened.error().message;
    ASSERT_TRUE(final_member_opened->floppy_set);
    EXPECT_EQ(final_member_opened->floppy_set->status, axk::app::ImageFloppySetStatus::incomplete);
    EXPECT_FALSE(final_member_opened->floppy_set->next_required_index);
    ASSERT_TRUE(images_->close(final_member_opened->image_id, "owner"));

    const auto first_opened = images_->open({"workspace", "disk01.ima", axk::app::ImageSourceKind::file}, "owner");
    ASSERT_TRUE(first_opened) << first_opened.error().message;
    ASSERT_TRUE(first_opened->floppy_set);
    EXPECT_EQ(first_opened->floppy_set->status, axk::app::ImageFloppySetStatus::incomplete);
    EXPECT_EQ(first_opened->floppy_set->next_required_index, 2U);
    EXPECT_TRUE(first_opened->companion_sources.empty());

    const auto rejected = images_->attach_companions(
        first_opened->image_id, "owner", first_opened->revision,
        {axk::app::CompanionSelectionKind::sources, {{"workspace", "source.hds", axk::app::ImageSourceKind::file}}});
    ASSERT_FALSE(rejected);
    const auto unchanged = images_->inspect(first_opened->image_id, "owner");
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    EXPECT_EQ(unchanged->revision, first_opened->revision);
    EXPECT_TRUE(unchanged->companion_sources.empty());

    const auto completed = images_->attach_companions(first_opened->image_id, "owner", first_opened->revision,
                                                      {axk::app::CompanionSelectionKind::immediate_siblings, {}});
    ASSERT_TRUE(completed) << completed.error().message;
    EXPECT_EQ(completed->format, "fat12-set");
    ASSERT_TRUE(completed->floppy_set);
    EXPECT_EQ(completed->floppy_set->status, axk::app::ImageFloppySetStatus::complete);
    EXPECT_FALSE(completed->floppy_set->next_required_index);
    ASSERT_EQ(completed->companion_sources.size(), 1U);
    EXPECT_EQ(completed->companion_sources.front(),
              (axk::app::ImageSourceRef{"workspace", "disk02.ima", axk::app::ImageSourceKind::file}));
    const auto completed_wave_data = images_->objects(completed->image_id, "owner", 32U, std::nullopt, "SMPL");
    ASSERT_TRUE(completed_wave_data) << completed_wave_data.error().message;
    ASSERT_EQ(completed_wave_data->items.size(), 1U);
    const auto completed_audition =
        images_->prepare_audition(completed->image_id, "owner", {completed_wave_data->items.front().id});
    ASSERT_TRUE(completed_audition) << completed_audition.error().message;
    EXPECT_GT(completed_audition->content_size_bytes, 44U);

    if (const auto artifact_output = environment_value("AXK_MULTIFLOPPY_ARTIFACT_OUTPUT")) {
        const std::filesystem::path artifact_path{*artifact_output};
        std::filesystem::create_directories(artifact_path.parent_path());
        std::error_code error;
        ASSERT_TRUE(std::filesystem::copy_file(retained->path, artifact_path,
                                               std::filesystem::copy_options::overwrite_existing, error))
            << error.message();
    }
}

} // namespace
