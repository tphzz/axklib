#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/package_operations.hpp"
#include "axklib/application/session_audio_export_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/package.hpp"
#include "axklib/writer.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    const std::vector<char> chars{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return {reinterpret_cast<const std::byte *>(chars.data()),
            reinterpret_cast<const std::byte *>(chars.data() + chars.size())};
}

void write_empty_target(const std::filesystem::path &path, std::string_view partition_name = "Target") {
    const auto manifest = axk::parse_hds_build_manifest(
        std::string{R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":")"} +
        std::string{partition_name} + R"(","volumes":[{"name":"Imported","waveforms":[],"samples":[]}]}]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto written = axk::write_hds_image(*manifest, path);
    ASSERT_TRUE(written) << written.error().message;
}

void write_mixed_root_source(const std::filesystem::path &path) {
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
    const auto audio_path = path.parent_path() / "mixed-root.wav";
    const auto written_audio = axk::write_wav_atomic(audio_path, waveform);
    ASSERT_TRUE(written_audio) << written_audio.error().message;

    axk::VolumeSpec volume;
    volume.name = "Mixed";
    volume.waveforms.push_back({"wave", "Wave", audio_path, 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60U;
    sample.key_high = 127U;
    volume.samples.push_back(sample);
    sample.name = "Direct";
    volume.samples.push_back(sample);
    sample.name = "Sample 2";
    volume.samples.push_back(sample);
    sample.name = "Direct 2";
    volume.samples.push_back(std::move(sample));
    volume.sample_banks.push_back({"Bank", {"Sample"}});
    volume.sample_banks.push_back({"Bank 2", {"Sample 2"}});
    volume.programs.push_back({1U, {{"SBAC", "Bank", 1U}, {"SBNK", "Direct", 2U}}});
    volume.programs.push_back({2U, {{"SBAC", "Bank 2", 1U}, {"SBNK", "Direct 2", 2U}}});

    const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};
    const auto written = axk::write_hds_image(manifest, path);
    ASSERT_TRUE(written) << written.error().message;
}

void write_object_directory(const std::filesystem::path &source, const std::filesystem::path &destination) {
    const auto media = axk::open_media(source);
    ASSERT_TRUE(media) << media.error().message;
    const auto objects = media->objects(axk::MediaObjectReadMode::complete);
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_FALSE(objects->empty());
    std::filesystem::create_directories(destination);
    for (std::size_t index = 0U; index < objects->size(); ++index) {
        std::ofstream output{destination / std::format("object-{:03}.bin", index), std::ios::binary};
        ASSERT_TRUE(output);
        const auto &payload = (*objects)[index].raw_payload;
        output.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(output);
    }
}

void write_split_object_directory(const std::filesystem::path &source, const std::filesystem::path &destination) {
    const auto media = axk::open_media(source);
    ASSERT_TRUE(media) << media.error().message;
    const auto objects = media->objects(axk::MediaObjectReadMode::complete);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto wave_data =
        std::ranges::find_if(*objects, [](const auto &object) { return object.decoded.header.raw_type == "SMPL"; });
    ASSERT_NE(wave_data, objects->end());
    const auto &header = wave_data->decoded.header;
    ASSERT_GT(header.payload_bytes_0x1c, 2U);

    const auto write_be32 = [](std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
        ASSERT_LE(offset + 4U, bytes.size());
        bytes[offset] = static_cast<std::byte>(value >> 24U);
        bytes[offset + 1U] = static_cast<std::byte>(value >> 16U);
        bytes[offset + 2U] = static_cast<std::byte>(value >> 8U);
        bytes[offset + 3U] = static_cast<std::byte>(value);
    };
    const auto write_file = [](const std::filesystem::path &path, std::span<const std::byte> payload) {
        std::ofstream output{path, std::ios::binary};
        ASSERT_TRUE(output);
        output.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(output);
    };

    const auto first_size = header.payload_bytes_0x1c / 2U;
    const auto second_size = header.payload_bytes_0x1c - first_size;
    std::vector<std::byte> first_segment(wave_data->raw_payload.begin(),
                                         wave_data->raw_payload.begin() +
                                             static_cast<std::ptrdiff_t>(header.header_size + first_size));
    write_be32(first_segment, 0x20U, first_size);
    write_be32(first_segment, 0x24U, 0U);
    std::vector<std::byte> second_segment(wave_data->raw_payload.begin(),
                                          wave_data->raw_payload.begin() +
                                              static_cast<std::ptrdiff_t>(header.header_size));
    second_segment.insert(second_segment.end(),
                          wave_data->raw_payload.begin() + static_cast<std::ptrdiff_t>(header.header_size + first_size),
                          wave_data->raw_payload.begin() +
                              static_cast<std::ptrdiff_t>(header.header_size + first_size + second_size));
    write_be32(second_segment, 0x20U, second_size);
    write_be32(second_segment, 0x24U, first_size);

    std::filesystem::create_directories(destination / "DISK1");
    std::filesystem::create_directories(destination / "DISK2");
    write_file(destination / "DISK1" / "SMP_TEST.001", first_segment);
    write_file(destination / "DISK2" / "SMP_TEST.001", second_segment);
}

class PackageOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-package-operations-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
        std::filesystem::copy_file(fixture_path(), root_ / "fixture.hds");
        write_empty_target(root_ / "target.hds");
        write_mixed_root_source(root_ / "mixed-roots.hds");
        write_object_directory(root_ / "fixture.hds", root_ / "objects");
        write_split_object_directory(root_ / "fixture.hds", root_ / "disk-set");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox);
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        uploads_ = std::make_unique<axk::app::UploadStore>(root_ / "uploads", 16U * 1024U * 1024U, 8U * 1024U * 1024U,
                                                           8U, 2U * 1024U * 1024U, std::chrono::minutes{5});
        registry_ = axk::app::make_operation_registry();
        ASSERT_TRUE(axk::app::bind_package_operations(registry_, *sandbox_, *uploads_));
        reservations_ = std::make_unique<axk::app::PathReservationCoordinator>();
        images_ = std::make_unique<axk::app::ImageSessionManager>(*sandbox_, 4U, 500U, std::chrono::minutes{15},
                                                                  std::chrono::steady_clock::now, reservations_.get());
        journals_ = std::make_unique<axk::app::AlterationJournalStore>(root_ / "journals");
        downloads_ = std::make_unique<axk::app::DownloadArchiveStore>(root_ / "downloads", 16U * 1024U * 1024U,
                                                                      8U * 1024U * 1024U, 8U, std::chrono::minutes{5});
        ASSERT_TRUE(journals_->storage_ready());
        ASSERT_TRUE(downloads_->storage_ready());
        ASSERT_TRUE(axk::app::bind_session_package_operations(registry_, *sandbox_, *uploads_, *images_, *journals_,
                                                              *downloads_));
        ASSERT_TRUE(axk::app::bind_session_audio_export_operations(registry_, *sandbox_, *images_, *downloads_));
    }

    void TearDown() override {
        images_.reset();
        downloads_.reset();
        journals_.reset();
        reservations_.reset();
        uploads_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    axk::app::OperationContext context() const {
        return {
            .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    std::unique_ptr<axk::app::UploadStore> uploads_;
    std::unique_ptr<axk::app::PathReservationCoordinator> reservations_;
    std::unique_ptr<axk::app::ImageSessionManager> images_;
    std::unique_ptr<axk::app::AlterationJournalStore> journals_;
    std::unique_ptr<axk::app::DownloadArchiveStore> downloads_;
    axk::app::OperationRegistry registry_;
};

TEST_F(PackageOperationsTest, ExportInspectUploadVerifyPlanAndApplyShareOneRegistryContract) {
    for (const auto operation : {"package.export", "package.inspect", "package.verify", "package.plan_import",
                                 "package.plan_import.release", "package.import"}) {
        EXPECT_TRUE(registry_.is_implemented(operation));
    }
    const auto exported = registry_.invoke(
        "package.export",
        {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
         {"output", {{"rootId", "workspace"}, {"relativePath", "sample"}}},
         {"roots",
          {{{"kind", "sbnk"}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}, {"objectName", "sine wave"}}}}},
        context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("output").at("relativePath"), "sample.axksbnk");
    EXPECT_TRUE(exported->at("payloadsVerified").get<bool>());
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "sample.axksbnk"));
    const auto wrong_extension = registry_.invoke(
        "package.export",
        {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
         {"output", {{"rootId", "workspace"}, {"relativePath", "sample.zip"}}},
         {"roots",
          {{{"kind", "sbnk"}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}, {"objectName", "sine wave"}}}}},
        context());
    ASSERT_FALSE(wrong_extension);
    EXPECT_EQ(wrong_extension.error().code, "package_extension_mismatch");

    const nlohmann::json file_input = {
        {"package", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "sample.axksbnk"}}}}}};
    const auto inspected = registry_.invoke("package.inspect", file_input, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->at("payloadsVerified").get<bool>());
    EXPECT_EQ(inspected->at("packageId"), exported->at("packageId"));
    EXPECT_TRUE(inspected->at("packageKind").is_string());
    EXPECT_TRUE(inspected->at("requiredExtension").is_string());
    ASSERT_FALSE(inspected->at("roots").empty());
    EXPECT_TRUE(inspected->at("roots").front().at("kind").is_string());
    ASSERT_EQ(inspected->at("relationships").size(), 1U);
    EXPECT_TRUE(inspected->at("relationships").front().at("edgeId").is_string());
    EXPECT_TRUE(inspected->at("relationships").front().at("sourceNodeId").is_string());
    EXPECT_TRUE(inspected->at("relationships").front().at("targetNodeId").is_string());
    EXPECT_EQ(inspected->at("relationships").front().at("role"), "SBNK_LEFT_MEMBER_TO_SMPL");
    EXPECT_EQ(inspected->at("relationships").front().at("ordinal"), 0U);
    std::uint64_t inspected_payload_bytes{};
    for (const auto &object : inspected->at("objects")) {
        inspected_payload_bytes += object.at("payloadSizeBytes").get<std::uint64_t>();
        EXPECT_TRUE(object.at("semanticSha256").is_null() || object.at("semanticSha256").is_string());
        EXPECT_TRUE(object.at("audioSha256").is_null() || object.at("audioSha256").is_string());
    }
    EXPECT_EQ(inspected->at("totalPayloadBytes"), inspected_payload_bytes);
    EXPECT_EQ(inspected->at("totalPayloadBytes"), exported->at("totalPayloadBytes"));

    const auto bytes = read_bytes(root_ / "sample.axksbnk");
    auto upload = uploads_->create({.owner_id = "owner",
                                    .filename = "dropped.axksbnk",
                                    .kind = axk::app::UploadKind::package,
                                    .media_type = "application/octet-stream",
                                    .declared_size = bytes.size(),
                                    .sha256 = std::nullopt});
    ASSERT_TRUE(upload) << upload.error().message;
    upload = uploads_->append(upload->reference, "owner", 0U, bytes);
    ASSERT_TRUE(upload) << upload.error().message;
    upload = uploads_->complete(upload->reference, "owner");
    ASSERT_TRUE(upload) << upload.error().message;
    const nlohmann::json upload_input = {{"package", {{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}};
    const auto verified = registry_.invoke("package.verify", upload_input, context());
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->at("payloadsVerified").get<bool>());
    EXPECT_EQ(verified->at("packageId"), exported->at("packageId"));
    EXPECT_EQ(verified->at("totalPayloadBytes"), inspected->at("totalPayloadBytes"));

    const auto import_request = nlohmann::json{
        {"target", {{"rootId", "workspace"}, {"relativePath", "target.hds"}}},
        {"output", {{"rootId", "workspace"}, {"relativePath", "imported.hds"}}},
        {"packages", {{{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}},
        {"destinations",
         {{{"packageIndex", 0U}, {"rootIndex", 0U}, {"partitionIndex", 0U}, {"volumeName", "Imported"}}}},
    };
    const auto planned = registry_.invoke("package.plan_import", import_request, context());
    ASSERT_TRUE(planned) << planned.error().message;
    ASSERT_TRUE(planned->at("valid").get<bool>());
    ASSERT_FALSE(planned->at("allocation").empty());
    EXPECT_GT(planned->at("allocation").front().at("additionalAllocatedBytes").get<std::uint64_t>(), 0U);
    EXPECT_EQ(planned->at("allocation").front().at("blockedObjectCount"), 0U);
    const auto accesses = registry_.path_accesses(
        "package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(accesses) << accesses.error().message;
    ASSERT_EQ(accesses->size(), 2U);
    EXPECT_EQ((*accesses)[0].reference, (axk::app::FileRef{"workspace", "target.hds"}));
    EXPECT_EQ((*accesses)[0].mode, axk::app::PathAccessMode::shared);
    EXPECT_EQ((*accesses)[1].reference, (axk::app::FileRef{"workspace", "imported.hds"}));
    EXPECT_EQ((*accesses)[1].mode, axk::app::PathAccessMode::exclusive);
    const auto reserved = registry_.invoke("package.plan_import", import_request, context());
    ASSERT_FALSE(reserved);
    EXPECT_EQ(reserved.error().code, "destination_reserved");

    auto releasable_request = import_request;
    releasable_request["output"]["relativePath"] = "released.hds";
    const auto releasable = registry_.invoke("package.plan_import", releasable_request, context());
    ASSERT_TRUE(releasable) << releasable.error().message;
    const auto released = registry_.invoke("package.plan_import.release",
                                           {{"planToken", releasable->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(released) << released.error().message;
    EXPECT_TRUE(released->at("released").get<bool>());
    EXPECT_FALSE(uploads_->remove(upload->reference, "owner"));
    EXPECT_FALSE(
        registry_.invoke("package.import", {{"planToken", releasable->at("planToken").get<std::string>()}}, context()));
    const auto replanned_released = registry_.invoke("package.plan_import", releasable_request, context());
    ASSERT_TRUE(replanned_released) << replanned_released.error().message;

    auto other = context();
    other.owner_id = "other";
    const auto denied =
        registry_.invoke("package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, other);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "package_plan_not_found");

    const auto applied =
        registry_.invoke("package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied->at("applied").get<bool>());
    ASSERT_TRUE(registry_.invoke("package.plan_import.release",
                                 {{"planToken", replanned_released->at("planToken").get<std::string>()}}, context()));
    EXPECT_TRUE(uploads_->remove(upload->reference, "owner"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "imported.hds"));
    EXPECT_NE(read_bytes(root_ / "imported.hds"), read_bytes(root_ / "target.hds"));
    EXPECT_FALSE(
        registry_.invoke("package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, context()));
}

TEST_F(PackageOperationsTest, SessionImportIsRevisionBoundJournaledAndExplicitlyReleasable) {
    const auto exported = registry_.invoke(
        "package.export",
        {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
         {"output", {{"rootId", "workspace"}, {"relativePath", "session-sample.axksbnk"}}},
         {"roots",
          {{{"kind", "sbnk"}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}, {"objectName", "sine wave"}}}}},
        context());
    ASSERT_TRUE(exported) << exported.error().message;
    const auto opened = images_->open({"workspace", "target.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;

    const auto request = nlohmann::json{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"package", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "session-sample.axksbnk"}}}}},
        {"partitionIndex", 0U},
        {"volumeName", "Imported"},
        {"renames", nlohmann::json::array()},
    };
    const auto abandoned = registry_.invoke("images.package_import.plan", request, context());
    ASSERT_TRUE(abandoned) << abandoned.error().message;
    ASSERT_TRUE(registry_.invoke("images.package_import.release",
                                 {{"planToken", abandoned->at("planToken").get<std::string>()}}, context()));
    const auto released = registry_.invoke("images.package_import",
                                           {{"planToken", abandoned->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(released);
    EXPECT_EQ(released.error().code, "package_plan_not_found");

    std::vector<nlohmann::json> diagnostics;
    auto operation_context = context();
    operation_context.diagnostic = [&diagnostics](const nlohmann::json &event) { diagnostics.push_back(event); };
    const auto planned = registry_.invoke("images.package_import.plan", request, operation_context);
    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_EQ(planned->at("imageId"), opened->image_id);
    EXPECT_EQ(planned->at("revision"), 1U);
    EXPECT_TRUE(planned->at("valid").get<bool>());
    ASSERT_FALSE(planned->at("actions").empty());
    auto replacement_request = request;
    replacement_request["replacePlanToken"] = planned->at("planToken");
    const auto replanned = registry_.invoke("images.package_import.plan", replacement_request, operation_context);
    ASSERT_TRUE(replanned) << replanned.error().message;
    EXPECT_NE(replanned->at("planToken"), planned->at("planToken"));
    const auto superseded = registry_.invoke("images.package_import",
                                             {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(superseded);
    EXPECT_EQ(superseded.error().code, "package_plan_not_found");
    ASSERT_TRUE(std::filesystem::remove(root_ / "session-sample.axksbnk"));
    EXPECT_TRUE(std::ranges::any_of(diagnostics, [](const auto &event) {
        return event.value("event", "") == "package_import_plan_phase" && event.value("phase", "") == "package" &&
               event.value("cacheHit", false);
    }));
    const auto applied = registry_.invoke("images.package_import",
                                          {{"planToken", replanned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->at("imageId"), opened->image_id);
    EXPECT_EQ(applied->at("revision"), 2U);
    EXPECT_TRUE(applied->at("applied").get<bool>());
    const auto refreshed = images_->inspect(opened->image_id, "owner");
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_EQ(refreshed->revision, 2U);
    EXPECT_GT(refreshed->object_count, opened->object_count);
}

TEST_F(PackageOperationsTest, SessionExportsExactSingleAndMultiRootPackagesToWorkspaceOrRetainedDownload) {
    const auto opened = images_->open({"workspace", "fixture.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto base =
        nlohmann::json{{"imageId", opened->image_id},
                       {"expectedRevision", opened->revision},
                       {"roots", {{{"kind", "VOLUME"}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}}}}};

    auto workspace_request = base;
    workspace_request["destination"] = {
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "exported-volume"}}},
        {"overwrite", false},
    };
    const auto workspace = registry_.invoke("images.package_export", workspace_request, context());
    ASSERT_TRUE(workspace) << workspace.error().message;
    EXPECT_EQ(workspace->at("destination"), "WORKSPACE");
    EXPECT_EQ(workspace->at("output").at("relativePath"), "exported-volume.axkvol");
    EXPECT_TRUE(workspace->at("download").is_null());
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "exported-volume.axkvol"));

    auto download_request = base;
    download_request["destination"] = {{"kind", "DOWNLOAD"}, {"filename", "local-volume.axkvol"}};
    const auto download = registry_.invoke("images.package_export", download_request, context());
    ASSERT_TRUE(download) << download.error().message;
    EXPECT_EQ(download->at("destination"), "DOWNLOAD");
    EXPECT_TRUE(download->at("output").is_null());
    const auto &retained = download->at("download");
    ASSERT_EQ(retained.at("filename"), "local-volume.axkvol");
    const auto content = downloads_->open({retained.at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(content) << content.error().message;
    const auto package = axk::open_portable_package(*content->reader, content->snapshot.filename);
    ASSERT_TRUE(package) << package.error().message;
    EXPECT_EQ(package->kind, axk::PackageKind::volume);
    auto wrong_extension = base;
    wrong_extension["destination"] = {{"kind", "DOWNLOAD"}, {"filename", "local-volume.zip"}};
    const auto rejected_extension = registry_.invoke("images.package_export", wrong_extension, context());
    ASSERT_FALSE(rejected_extension);
    EXPECT_EQ(rejected_extension.error().code, "package_extension_mismatch");

    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto sample = std::ranges::find(objects->items, "SBNK", &axk::app::ImageObjectItem::type);
    const auto wave_data = std::ranges::find(objects->items, "SMPL", &axk::app::ImageObjectItem::type);
    ASSERT_NE(sample, objects->items.end());
    ASSERT_NE(wave_data, objects->items.end());

    const nlohmann::json selected_roots{
        {{"kind", "SBNK"}, {"objectId", sample->id}},
        {{"kind", "SMPL"}, {"objectId", wave_data->id}},
    };
    const auto selected =
        registry_.invoke("images.package_export",
                         {{"imageId", opened->image_id},
                          {"expectedRevision", opened->revision},
                          {"roots", selected_roots},
                          {"destination",
                           {{"kind", "WORKSPACE"},
                            {"output", {{"rootId", "workspace"}, {"relativePath", "selected-objects"}}},
                            {"overwrite", false}}}},
                         context());
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_EQ(selected->at("packageKind"), "bundle");
    EXPECT_EQ(selected->at("output").at("relativePath"), "selected-objects.axkpkg");
    const auto selected_package = axk::open_portable_package(root_ / "selected-objects.axkpkg");
    ASSERT_TRUE(selected_package) << selected_package.error().message;
    EXPECT_EQ(selected_package->kind, axk::PackageKind::bundle);
    EXPECT_EQ(selected_package->roots.size(), 2U);
    EXPECT_EQ(selected_package->nodes.size(), 2U);
    EXPECT_EQ(std::ranges::count(selected_package->nodes, std::string{"SMPL"}, &axk::PackageNode::object_type), 1);

    const auto wrong_type = registry_.invoke("images.package_export",
                                             {{"imageId", opened->image_id},
                                              {"expectedRevision", opened->revision},
                                              {"roots", {{{"kind", "SMPL"}, {"objectId", sample->id}}}},
                                              {"destination", {{"kind", "DOWNLOAD"}, {"filename", "wrong.axksmpl"}}}},
                                             context());
    ASSERT_FALSE(wrong_type);
    EXPECT_EQ(wrong_type.error().code, "package_operation_failed");

    const auto duplicate = registry_.invoke(
        "images.package_export",
        {{"imageId", opened->image_id},
         {"expectedRevision", opened->revision},
         {"roots", {{{"kind", "SBNK"}, {"objectId", sample->id}}, {{"kind", "SBNK"}, {"objectId", sample->id}}}},
         {"destination", {{"kind", "DOWNLOAD"}, {"filename", "duplicate.axkpkg"}}}},
        context());
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "invalid_request");
}

TEST_F(PackageOperationsTest, SessionInspectsAndExportsSfzToWorkspaceOrRetainedTar) {
    const auto opened = images_->open({"workspace", "fixture.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto sample = std::ranges::find(objects->items, "SBNK", &axk::app::ImageObjectItem::type);
    ASSERT_NE(sample, objects->items.end());
    const nlohmann::json roots{{{"kind", "SBNK"}, {"objectId", sample->id}}};
    const auto base =
        nlohmann::json{{"imageId", opened->image_id}, {"expectedRevision", opened->revision}, {"roots", roots}};

    const auto inspected = registry_.invoke("images.audio_export.inspect", base, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->at("sfzEligible").get<bool>());
    EXPECT_EQ(inspected->at("sampleCount"), 1U);
    EXPECT_EQ(inspected->at("waveDataCount"), 1U);
    EXPECT_EQ(inspected->at("sfzFileCount"), 1U);
    EXPECT_FALSE(inspected->at("defaultDirectoryName").get<std::string>().empty());

    auto workspace_request = base;
    workspace_request["format"] = "SFZ";
    workspace_request["destination"] = {
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "sfz-workspace"}}},
    };
    const auto workspace = registry_.invoke("images.audio_export", workspace_request, context());
    ASSERT_TRUE(workspace) << workspace.error().message;
    EXPECT_EQ(workspace->at("destination"), "WORKSPACE");
    EXPECT_EQ(workspace->at("format"), "SFZ");
    EXPECT_GE(workspace->at("fileCount").get<std::size_t>(), 2U);
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "sfz-workspace"));
    EXPECT_NE(std::ranges::distance(std::filesystem::recursive_directory_iterator{root_ / "sfz-workspace"},
                                    std::filesystem::recursive_directory_iterator{}),
              0);

    auto download_request = base;
    download_request["format"] = "SFZ";
    download_request["destination"] = {{"kind", "DOWNLOAD"}, {"directoryName", "Local SFZ"}};
    const auto download = registry_.invoke("images.audio_export", download_request, context());
    ASSERT_TRUE(download) << download.error().message;
    ASSERT_EQ(download->at("destination"), "DOWNLOAD");
    const auto &retained = download->at("download");
    ASSERT_EQ(retained.at("filename"), "Local SFZ.tar");
    const auto content = downloads_->open({retained.at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(content->snapshot.media_type, "application/x-tar");
    EXPECT_GT(content->snapshot.entry_count, 1U);
}

TEST_F(PackageOperationsTest, SessionExportsAnAxkObjectDirectoryAsAVolumePackage) {
    const auto opened =
        images_->open({"workspace", "objects", axk::app::ImageSourceKind::axk_object_directory}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto content = images_->content(opened->image_id, "owner", 64U);
    ASSERT_TRUE(content) << content.error().message;
    ASSERT_EQ(content->items.size(), 1U);
    const auto &volume = content->items.front();
    ASSERT_EQ(volume.kind, "volume");
    ASSERT_TRUE(volume.partition_index);

    const auto exported = registry_.invoke(
        "images.package_export",
        {{"imageId", opened->image_id},
         {"expectedRevision", opened->revision},
         {"roots", {{{"kind", "VOLUME"}, {"partitionIndex", *volume.partition_index}, {"volumeName", volume.name}}}},
         {"destination",
          {{"kind", "WORKSPACE"},
           {"output", {{"rootId", "workspace"}, {"relativePath", "object-directory"}}},
           {"overwrite", false}}}},
        context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("packageKind"), "volume");
    EXPECT_EQ(exported->at("requiredExtension"), ".axkvol");
    EXPECT_EQ(exported->at("sourceMediaKind"), "axk-object-directory");
    EXPECT_EQ(exported->at("output").at("relativePath"), "object-directory.axkvol");
    EXPECT_TRUE(exported->at("payloadsVerified").get<bool>());

    const auto package = axk::open_portable_package(root_ / "object-directory.axkvol");
    ASSERT_TRUE(package) << package.error().message;
    EXPECT_EQ(package->kind, axk::PackageKind::volume);
    ASSERT_EQ(package->roots.size(), 1U);
    EXPECT_EQ(package->roots.front().display_name, "Object directory");
    EXPECT_EQ(package->nodes.size(), opened->object_count);
}

TEST_F(PackageOperationsTest, SessionExportRequestsCompanionDisksAndSucceedsAfterExplicitAttachment) {
    const auto opened =
        images_->open({"workspace", "disk-set/DISK2", axk::app::ImageSourceKind::axk_object_directory}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto content = images_->content(opened->image_id, "owner", 64U);
    ASSERT_TRUE(content) << content.error().message;
    ASSERT_EQ(content->items.size(), 1U);
    const auto &volume = content->items.front();
    ASSERT_TRUE(volume.partition_index);
    const nlohmann::json roots{
        {{"kind", "VOLUME"}, {"partitionIndex", *volume.partition_index}, {"volumeName", volume.name}}};
    const auto destination = nlohmann::json{
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "split-object-directory"}}},
        {"overwrite", false},
    };

    const auto rejected = registry_.invoke("images.package_export",
                                           {{"imageId", opened->image_id},
                                            {"expectedRevision", opened->revision},
                                            {"roots", roots},
                                            {"destination", destination}},
                                           context());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "companion_disks_required") << rejected.error().message;

    const auto attached = images_->attach_companion_directories(
        opened->image_id, "owner", opened->revision,
        {axk::app::CompanionDirectorySelectionKind::directories, {{"workspace", "disk-set/DISK1"}}});
    ASSERT_TRUE(attached) << attached.error().message;
    const auto exported = registry_.invoke("images.package_export",
                                           {{"imageId", attached->image_id},
                                            {"expectedRevision", attached->revision},
                                            {"roots", roots},
                                            {"destination", destination}},
                                           context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("packageKind"), "volume");
    EXPECT_EQ(exported->at("output").at("relativePath"), "split-object-directory.axkvol");
}

TEST_F(PackageOperationsTest, SessionExportCombinesEveryPortableObjectRootKindWithoutDuplicatingTheGraph) {
    const auto opened = images_->open({"workspace", "mixed-roots.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;

    std::vector<std::string> sample_bank_ids;
    for (const auto &object : objects->items) {
        if (object.type == "SBAC")
            sample_bank_ids.push_back(object.id);
    }
    ASSERT_EQ(sample_bank_ids.size(), 2U);
    const nlohmann::json homogeneous_roots{
        {{"kind", "SBAC"}, {"objectId", sample_bank_ids[0]}},
        {{"kind", "SBAC"}, {"objectId", sample_bank_ids[1]}},
    };
    const auto homogeneous =
        registry_.invoke("images.package_export",
                         {{"imageId", opened->image_id},
                          {"expectedRevision", opened->revision},
                          {"roots", homogeneous_roots},
                          {"destination",
                           {{"kind", "WORKSPACE"},
                            {"output", {{"rootId", "workspace"}, {"relativePath", "sample-banks"}}},
                            {"overwrite", false}}}},
                         context());
    ASSERT_TRUE(homogeneous) << homogeneous.error().message;
    EXPECT_EQ(homogeneous->at("packageKind"), "sbac");
    EXPECT_EQ(homogeneous->at("requiredExtension"), ".axksbac");
    EXPECT_EQ(homogeneous->at("output").at("relativePath"), "sample-banks.axksbac");
    const auto sample_banks = axk::open_portable_package(root_ / "sample-banks.axksbac");
    ASSERT_TRUE(sample_banks) << sample_banks.error().message;
    EXPECT_EQ(sample_banks->kind, axk::PackageKind::sbac);
    EXPECT_EQ(sample_banks->roots.size(), 2U);

    const auto object_id = [&](std::string_view type) {
        const auto found = std::ranges::find(objects->items, type, &axk::app::ImageObjectItem::type);
        EXPECT_NE(found, objects->items.end());
        return found == objects->items.end() ? std::string{} : found->id;
    };
    const nlohmann::json roots{
        {{"kind", "SMPL"}, {"objectId", object_id("SMPL")}},
        {{"kind", "PROGRAM"}, {"objectId", object_id("PROG")}},
        {{"kind", "SBNK"}, {"objectId", object_id("SBNK")}},
        {{"kind", "SBAC"}, {"objectId", object_id("SBAC")}},
    };
    const auto exported = registry_.invoke("images.package_export",
                                           {{"imageId", opened->image_id},
                                            {"expectedRevision", opened->revision},
                                            {"roots", roots},
                                            {"destination",
                                             {{"kind", "WORKSPACE"},
                                              {"output", {{"rootId", "workspace"}, {"relativePath", "mixed-roots"}}},
                                              {"overwrite", false}}}},
                                           context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("packageKind"), "bundle");
    EXPECT_EQ(exported->at("output").at("relativePath"), "mixed-roots.axkpkg");

    const auto package = axk::open_portable_package(root_ / "mixed-roots.axkpkg");
    ASSERT_TRUE(package) << package.error().message;
    EXPECT_EQ(package->roots.size(), 4U);
    EXPECT_EQ(package->nodes.size(), 5U);
    EXPECT_EQ(std::ranges::count(package->nodes, std::string{"PROG"}, &axk::PackageNode::object_type), 1);
    EXPECT_EQ(std::ranges::count(package->nodes, std::string{"SBAC"}, &axk::PackageNode::object_type), 1);
    EXPECT_EQ(std::ranges::count(package->nodes, std::string{"SBNK"}, &axk::PackageNode::object_type), 2);
    EXPECT_EQ(std::ranges::count(package->nodes, std::string{"SMPL"}, &axk::PackageNode::object_type), 1);
}

TEST_F(PackageOperationsTest, RejectsSequenceRootsAndUploadOwnershipMismatch) {
    const auto sequence = registry_.invoke("package.export",
                                           {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
                                            {"output", {{"rootId", "workspace"}, {"relativePath", "sequence.axkseq"}}},
                                            {"roots", {{{"kind", "sequence"}, {"objectName", "Sequence"}}}}},
                                           context());
    ASSERT_FALSE(sequence);
    EXPECT_EQ(sequence.error().code, "unsupported_package_root");

    auto upload = uploads_->create({.owner_id = "owner",
                                    .filename = "private.axkvol",
                                    .kind = axk::app::UploadKind::package,
                                    .media_type = "application/octet-stream",
                                    .declared_size = 1U,
                                    .sha256 = std::nullopt});
    ASSERT_TRUE(upload) << upload.error().message;
    const std::array byte{std::byte{0}};
    ASSERT_TRUE(uploads_->append(upload->reference, "owner", 0U, byte));
    ASSERT_TRUE(uploads_->complete(upload->reference, "owner"));
    auto other = context();
    other.owner_id = "other";
    const auto denied = registry_.invoke(
        "package.inspect", {{"package", {{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}}, other);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "upload_not_found");
}

TEST_F(PackageOperationsTest, MapsCanonicalFriendlyRootNamesAndRejectsObsoleteNames) {
    const auto export_kind = [&](std::string_view kind, std::string_view name, std::string_view output) {
        return registry_.invoke(
            "package.export",
            {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
             {"output", {{"rootId", "workspace"}, {"relativePath", output}}},
             {"roots", {{{"kind", kind}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}, {"objectName", name}}}}},
            context());
    };

    const auto sample_bank = export_kind("sample-bank", "New SmpBank", "sample-bank");
    ASSERT_TRUE(sample_bank) << sample_bank.error().message;
    EXPECT_EQ(sample_bank->at("packageKind"), "sbac");

    const auto obsolete_bank_group = export_kind("bank-group", "New SmpBank", "bank-group");
    ASSERT_FALSE(obsolete_bank_group);
    EXPECT_EQ(obsolete_bank_group.error().code, "unsupported_package_root");

    const auto sample = export_kind("sample", "sine wave", "sample");
    ASSERT_TRUE(sample) << sample.error().message;
    EXPECT_EQ(sample->at("packageKind"), "sbnk");

    const auto wave_data = export_kind("wave-data", "sine wave", "wave-data");
    ASSERT_TRUE(wave_data) << wave_data.error().message;
    EXPECT_EQ(wave_data->at("packageKind"), "smpl");
}

TEST_F(PackageOperationsTest, PlanTokenRejectsChangedPackageAndTargetFilesWithoutPublication) {
    const auto export_package = [&](std::string_view name, std::string_view output) {
        return registry_.invoke(
            "package.export",
            {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
             {"output", {{"rootId", "workspace"}, {"relativePath", output}}},
             {"roots",
              {{{"kind", "sbnk"}, {"partitionIndex", 0U}, {"volumeName", "New Volume"}, {"objectName", name}}}}},
            context());
    };
    const auto sine = export_package("sine wave", "sine");
    ASSERT_TRUE(sine) << sine.error().message;
    const auto square = export_package("square", "square");
    ASSERT_TRUE(square) << square.error().message;
    const auto sine_bytes = read_bytes(root_ / "sine.axksbnk");

    const auto plan_request = [&](std::string_view target, std::string_view output, std::string_view package) {
        return nlohmann::json{
            {"target", {{"rootId", "workspace"}, {"relativePath", target}}},
            {"output", {{"rootId", "workspace"}, {"relativePath", output}}},
            {"packages", {{{"fileRef", {{"rootId", "workspace"}, {"relativePath", package}}}}}},
            {"destinations",
             {{{"packageIndex", 0U}, {"rootIndex", 0U}, {"partitionIndex", 0U}, {"volumeName", "Imported"}}}}};
    };

    const auto package_plan = registry_.invoke(
        "package.plan_import", plan_request("target.hds", "stale-package.hds", "sine.axksbnk"), context());
    ASSERT_TRUE(package_plan) << package_plan.error().message;
    std::filesystem::copy_file(root_ / "square.axksbnk", root_ / "sine.axksbnk",
                               std::filesystem::copy_options::overwrite_existing);
    const auto stale_package = registry_.invoke(
        "package.import", {{"planToken", package_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(stale_package);
    EXPECT_EQ(stale_package.error().code, "package_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale-package.hds"));

    std::ofstream sine_output{root_ / "sine.axksbnk", std::ios::binary | std::ios::trunc};
    sine_output.write(reinterpret_cast<const char *>(sine_bytes.data()),
                      static_cast<std::streamsize>(sine_bytes.size()));
    sine_output.close();
    std::filesystem::copy_file(root_ / "target.hds", root_ / "stale-target.hds");
    write_empty_target(root_ / "changed-target.hds", "Changed");
    const auto target_plan = registry_.invoke(
        "package.plan_import", plan_request("stale-target.hds", "stale-target-output.hds", "sine.axksbnk"), context());
    ASSERT_TRUE(target_plan) << target_plan.error().message;
    std::filesystem::copy_file(root_ / "changed-target.hds", root_ / "stale-target.hds",
                               std::filesystem::copy_options::overwrite_existing);
    const auto stale_target =
        registry_.invoke("package.import", {{"planToken", target_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(stale_target);
    EXPECT_EQ(stale_target.error().code, "package_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale-target-output.hds"));
}

} // namespace
