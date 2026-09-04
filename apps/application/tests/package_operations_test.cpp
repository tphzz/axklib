#include <algorithm>
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
#include "axklib/application/session_sequence_operations.hpp"
#include "axklib/application/session_volume_package_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/package.hpp"
#include "axklib/sequence.hpp"
#include "axklib/writer.hpp"

#include "a3k_test_fixture.hpp"

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

void write_sequence_object(const std::filesystem::path &path, std::string_view name) {
    const std::array smf{
        std::byte{'M'}, std::byte{'T'},  std::byte{'h'},  std::byte{'d'},  std::byte{0},   std::byte{0},
        std::byte{0},   std::byte{6},    std::byte{0},    std::byte{0},    std::byte{0},   std::byte{1},
        std::byte{0},   std::byte{96},   std::byte{'M'},  std::byte{'T'},  std::byte{'r'}, std::byte{'k'},
        std::byte{0},   std::byte{0},    std::byte{0},    std::byte{12},   std::byte{0},   std::byte{0x90},
        std::byte{60},  std::byte{100},  std::byte{96},   std::byte{0x80}, std::byte{60},  std::byte{0},
        std::byte{0},   std::byte{0xff}, std::byte{0x2f}, std::byte{0},
    };
    const auto sequence = axk::smf0_to_current_sequence(smf, name, axk::SequenceSystemExclusivePolicy::reject);
    ASSERT_TRUE(sequence) << sequence.error().message;
    std::ofstream output{path, std::ios::binary};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(sequence->data()), static_cast<std::streamsize>(sequence->size()));
    ASSERT_TRUE(output);
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
    sample.parameters.root_key = 60U;
    sample.parameters.key_high = 127U;
    volume.samples.push_back(sample);
    sample.name = "Direct";
    volume.samples.push_back(sample);
    sample.name = "Sample 2";
    volume.samples.push_back(sample);
    sample.name = "Direct 2";
    volume.samples.push_back(std::move(sample));
    volume.sample_banks.push_back({"Bank", {"Sample"}});
    volume.sample_banks.push_back({"Bank 2", {"Sample 2"}});
    volume.programs.push_back({1U, "Pgm 001", {{"SBAC", "Bank", 1U}, {"SBNK", "Direct", 2U}}});
    volume.programs.push_back({2U, "Pgm 002", {{"SBAC", "Bank 2", 1U}, {"SBNK", "Direct 2", 2U}}});

    const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};
    const auto written = axk::write_hds_image(manifest, path);
    ASSERT_TRUE(written) << written.error().message;
}

void write_selected_wav_source(const std::filesystem::path &path) {
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
    const auto audio_path = path.parent_path() / "selected-wav.wav";
    const auto written_audio = axk::write_wav_atomic(audio_path, waveform);
    ASSERT_TRUE(written_audio) << written_audio.error().message;

    axk::VolumeSpec volume;
    volume.name = "Selected WAV";
    volume.waveforms.push_back({"mono", "Mono Wave", audio_path, 60U, {}});
    volume.waveforms.push_back({"left", "Stereo Left", audio_path, 60U, {}});
    volume.waveforms.push_back({"right", "Stereo Right", audio_path, 60U, {}});
    axk::SampleSpec mono;
    mono.name = "Mono Sample";
    mono.waveform_id = "mono";
    mono.parameters.root_key = 60U;
    mono.parameters.key_high = 127U;
    volume.samples.push_back(std::move(mono));
    axk::SampleSpec stereo;
    stereo.name = "Stereo Sample";
    stereo.waveform_id = "left";
    stereo.right_waveform_id = "right";
    stereo.parameters.root_key = 60U;
    stereo.parameters.key_high = 127U;
    volume.samples.push_back(std::move(stereo));

    const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};
    const auto written = axk::write_hds_image(manifest, path);
    ASSERT_TRUE(written) << written.error().message;
}

void write_invalid_stereo_object_directory(const std::filesystem::path &source,
                                           const std::filesystem::path &destination) {
    const auto media = axk::open_media(source);
    ASSERT_TRUE(media) << media.error().message;
    const auto objects = media->objects(axk::MediaObjectReadMode::complete);
    ASSERT_TRUE(objects) << objects.error().message;
    std::filesystem::create_directories(destination);
    bool changed{};
    for (std::size_t index = 0U; index < objects->size(); ++index) {
        auto payload = (*objects)[index].raw_payload;
        if ((*objects)[index].decoded.header.raw_type == "SBNK" &&
            (*objects)[index].decoded.header.name == "Stereo Sample") {
            ASSERT_GE(payload.size(), 0xa8U);
            std::ranges::copy_n(payload.begin() + 0x78, 16U, payload.begin() + 0x88);
            std::ranges::copy_n(payload.begin() + 0xa0, 4U, payload.begin() + 0xa4);
            const auto decoded = axk::decode_object(payload);
            ASSERT_TRUE(decoded) << decoded.error().message;
            const auto *sample = std::get_if<axk::CurrentSbnk>(&decoded->payload);
            ASSERT_NE(sample, nullptr);
            ASSERT_TRUE(sample->right);
            changed = true;
        }
        std::ofstream output{destination / std::format("object-{:03}.bin", index), std::ios::binary};
        ASSERT_TRUE(output);
        output.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(output);
    }
    ASSERT_TRUE(changed);
}

void write_batch_volume_source(const std::filesystem::path &path) {
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
    const auto audio_path = path.parent_path() / "batch-volume.wav";
    const auto written_audio = axk::write_wav_atomic(audio_path, waveform);
    ASSERT_TRUE(written_audio) << written_audio.error().message;

    const auto volume = [&](std::string waveform_name, std::string sample_name) {
        axk::VolumeSpec result;
        result.name = "Duplicate";
        result.waveforms.push_back({"wave", std::move(waveform_name), audio_path, 60U, {}});
        axk::SampleSpec sample;
        sample.name = std::move(sample_name);
        sample.waveform_id = "wave";
        sample.parameters.root_key = 60U;
        sample.parameters.key_high = 127U;
        result.samples.push_back(std::move(sample));
        return result;
    };
    axk::VolumeSpec empty;
    empty.name = "Empty";
    const axk::HdsBuildManifest manifest{
        "1.0",
        4U * 1024U * 1024U,
        {{"Batch", {volume("First Wave", "First Sample"), std::move(empty), volume("Second Wave", "Second Sample")}}}};
    const auto written = axk::write_hds_image(manifest, path);
    ASSERT_TRUE(written) << written.error().message;
}

void write_audio_source_with_orphan_wave_data(const std::filesystem::path &path) {
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 2U;
    waveform.pcm = {std::byte{0}, std::byte{0}, std::byte{0xe8}, std::byte{3}};
    const auto audio_path = path.parent_path() / "orphan-source.wav";
    const auto written_audio = axk::write_wav_atomic(audio_path, waveform);
    ASSERT_TRUE(written_audio) << written_audio.error().message;

    axk::VolumeSpec volume;
    volume.name = "Fatal";
    volume.waveforms.push_back({"linked-wave", "Linked Wave", audio_path, 60U, {}});
    volume.waveforms.push_back({"orphan-wave", "Orphan Wave", audio_path, 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Linked Sample";
    sample.waveform_id = "linked-wave";
    sample.parameters.root_key = 60U;
    sample.parameters.key_high = 127U;
    volume.samples.push_back(std::move(sample));

    const axk::HdsBuildManifest manifest{"1.0", 2U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};
    const auto written = axk::write_hds_image(manifest, path);
    ASSERT_TRUE(written) << written.error().message;
}

void make_first_program_assignment_context_only(const std::filesystem::path &path) {
    const auto media = axk::open_media(path);
    ASSERT_TRUE(media) << media.error().message;
    const auto *sfs = std::get_if<axk::Container>(&media->storage());
    ASSERT_NE(sfs, nullptr);
    ASSERT_FALSE(sfs->partitions().empty());
    const auto &partition = sfs->partitions().front();
    const auto program =
        std::ranges::find_if(partition.records, [](const auto &record) { return record.object_type == "PROG"; });
    ASSERT_NE(program, partition.records.end());
    ASSERT_EQ(program->extents.size(), 1U);
    const auto row_offset =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(program->extents.front().cluster_offset) * partition.sectors_per_cluster) *
            512U +
        0x120U;
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    image.seekp(static_cast<std::streamoff>(row_offset + 0x0fU));
    image.put('*');
    const auto context_offset = row_offset + 0x38U;
    image.seekp(static_cast<std::streamoff>(context_offset));
    image.write("Bank            ", 16);
    image.seekp(static_cast<std::streamoff>(context_offset + 0x14U));
    image.put(static_cast<char>(0x11));
    ASSERT_TRUE(image);
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
        write_sequence_object(root_ / "sequence.bin", "Sequence");
        write_empty_target(root_ / "target.hds");
        write_mixed_root_source(root_ / "mixed-roots.hds");
        write_selected_wav_source(root_ / "selected-wav.hds");
        write_invalid_stereo_object_directory(root_ / "selected-wav.hds", root_ / "invalid-stereo-objects");
        write_batch_volume_source(root_ / "batch-volumes.hds");
        write_object_directory(root_ / "fixture.hds", root_ / "objects");
        write_split_object_directory(root_ / "fixture.hds", root_ / "disk-set");
        axk::app::test::write_a3k_archive(root_ / "fixture.hds", root_ / "archive.a3k");
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
        ASSERT_TRUE(axk::app::bind_session_volume_package_operations(registry_, *sandbox_, *images_, *downloads_));
        ASSERT_TRUE(axk::app::bind_session_audio_export_operations(registry_, *sandbox_, *images_, *downloads_));
        ASSERT_TRUE(axk::app::bind_session_sequence_operations(registry_, *sandbox_, *images_, *downloads_));
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

    std::string volume_content_id(const axk::app::ImageSessionSummary &session, std::string_view name) {
        const auto read = images_->begin_read(session.image_id, "owner", session.revision);
        EXPECT_TRUE(read) << read.error().message;
        if (!read)
            return {};
        const auto found = std::ranges::find_if(read->volume_scopes_by_id,
                                                [&](const auto &entry) { return entry.second.display_name == name; });
        EXPECT_NE(found, read->volume_scopes_by_id.end());
        return found == read->volume_scopes_by_id.end() ? std::string{} : found->first;
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
    EXPECT_TRUE(planned->at("programAssignmentAdjustments").empty());
    EXPECT_TRUE(planned->at("programSlotPlacements").empty());
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
    EXPECT_TRUE(applied->at("programAssignmentAdjustments").empty());
    ASSERT_TRUE(registry_.invoke("package.plan_import.release",
                                 {{"planToken", replanned_released->at("planToken").get<std::string>()}}, context()));
    EXPECT_TRUE(uploads_->remove(upload->reference, "owner"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "imported.hds"));
    EXPECT_NE(read_bytes(root_ / "imported.hds"), read_bytes(root_ / "target.hds"));
    EXPECT_FALSE(
        registry_.invoke("package.import", {{"planToken", planned->at("planToken").get<std::string>()}}, context()));
}

TEST_F(PackageOperationsTest, SessionImportIsRevisionBoundJournaledAndExplicitlyReleasable) {
    const auto exported =
        registry_.invoke("package.export",
                         {{"source", {{"rootId", "workspace"}, {"relativePath", "mixed-roots.hds"}}},
                          {"output", {{"rootId", "workspace"}, {"relativePath", "session-volume.axkvol"}}},
                          {"roots", {{{"kind", "volume"}, {"partitionIndex", 0U}, {"volumeName", "Mixed"}}}}},
                         context());
    ASSERT_TRUE(exported) << exported.error().message;
    const auto opened = images_->open({"workspace", "target.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;

    const auto request = nlohmann::json{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"packages", {{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "session-volume.axkvol"}}}}}},
        {"destination", {{"kind", "EXISTING_VOLUME"}, {"partitionIndex", 0U}, {"volumeName", "Imported"}}},
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
    EXPECT_TRUE(planned->at("programAssignmentAdjustments").empty());
    EXPECT_TRUE(planned->at("programSlotPlacements").empty());
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
    ASSERT_TRUE(std::filesystem::remove(root_ / "session-volume.axkvol"));
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
    EXPECT_TRUE(applied->at("programAssignmentAdjustments").empty());
    EXPECT_TRUE(applied->at("programSlotPlacements").empty());
    const auto refreshed = images_->inspect(opened->image_id, "owner");
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_EQ(refreshed->revision, 2U);
    EXPECT_GT(refreshed->object_count, opened->object_count);
}

TEST_F(PackageOperationsTest, SessionBatchImportCreatesUniquelyNamedVolumesAtomicallyFromPlacementHints) {
    for (const auto filename : {"batch-one.axkvol", "batch-two.axkvol"}) {
        const auto exported =
            registry_.invoke("package.export",
                             {{"source", {{"rootId", "workspace"}, {"relativePath", "mixed-roots.hds"}}},
                              {"output", {{"rootId", "workspace"}, {"relativePath", filename}}},
                              {"roots", {{{"kind", "volume"}, {"partitionIndex", 0U}, {"volumeName", "Mixed"}}}}},
                             context());
        ASSERT_TRUE(exported) << exported.error().message;
    }
    const auto opened = images_->open({"workspace", "target.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;

    const auto request = nlohmann::json{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"packages",
         {{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "batch-one.axkvol"}}}},
          {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "batch-two.axkvol"}}}}}},
        {"destination",
         {{"kind", "CREATE_VOLUMES_FROM_HINTS"},
          {"partitionIndex", 0U},
          {"volumeNameOverrides", nlohmann::json::array()}}},
        {"renames", nlohmann::json::array()},
        {"programSlotAssignments", nlohmann::json::array()},
        {"opaqueSequenceDecisions", nlohmann::json::array()},
    };
    const auto planned = registry_.invoke("images.package_import.plan", request, context());
    ASSERT_TRUE(planned) << planned.error().message;
    ASSERT_TRUE(planned->at("valid").get<bool>());
    ASSERT_EQ(planned->at("packages").size(), 2U);
    EXPECT_EQ(planned->at("packages").at(0).at("destinationVolumeName"), "Mixed");
    EXPECT_EQ(planned->at("packages").at(1).at("destinationVolumeName"), "Mixed 2");
    EXPECT_EQ(planned->at("packages").at(0).at("objectCounts").at("programs"), 2U);
    EXPECT_EQ(planned->at("packages").at(0).at("objectCounts").at("sampleBanks"), 2U);
    EXPECT_EQ(planned->at("packages").at(0).at("objectCounts").at("samples"), 4U);
    EXPECT_EQ(planned->at("packages").at(0).at("objectCounts").at("waveData"), 1U);
    ASSERT_EQ(planned->at("sfsIndexCapacity").size(), 1U);
    const auto &capacity = planned->at("sfsIndexCapacity").front();
    EXPECT_EQ(capacity.at("recordsPerIndexBlock"), 14U);
    EXPECT_EQ(capacity.at("requiredRecordSlots"), capacity.at("allocatedRecordSlots"));
    EXPECT_EQ(capacity.at("shortfallRecordSlots"), 0U);
    ASSERT_EQ(capacity.at("packages").size(), 2U);
    EXPECT_EQ(capacity.at("packages").at(0).at("volumeScaffoldingRecordSlots"), 6U);
    EXPECT_EQ(capacity.at("packages").at(1).at("volumeScaffoldingRecordSlots"), 6U);

    auto replacement_request = request;
    replacement_request["replacePlanToken"] = planned->at("planToken");
    replacement_request["destination"]["volumeNameOverrides"] =
        nlohmann::json::array({{{"packageIndex", 1U}, {"volumeName", "Percussion"}}});
    const auto replanned = registry_.invoke("images.package_import.plan", replacement_request, context());
    ASSERT_TRUE(replanned) << replanned.error().message;
    EXPECT_EQ(replanned->at("packages").at(0).at("destinationVolumeName"), "Mixed");
    EXPECT_EQ(replanned->at("packages").at(1).at("destinationVolumeName"), "Percussion");
    const auto superseded = registry_.invoke("images.package_import",
                                             {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(superseded);
    EXPECT_EQ(superseded.error().code, "package_plan_not_found");

    const auto applied = registry_.invoke("images.package_import",
                                          {{"planToken", replanned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->at("revision"), opened->revision + 1U);
    EXPECT_EQ(applied->size(), 12U);
    EXPECT_FALSE(applied->contains("sfsIndexCapacity"));
    const auto refreshed = images_->inspect(opened->image_id, "owner");
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_FALSE(volume_content_id(*refreshed, "Mixed").empty());
    EXPECT_FALSE(volume_content_id(*refreshed, "Percussion").empty());
}

TEST_F(PackageOperationsTest, SessionBatchImportCreatesOneSharedVolumeFromMultiplePackages) {
    for (const auto filename : {"shared-one.axkvol", "shared-two.axkvol"}) {
        const auto exported =
            registry_.invoke("package.export",
                             {{"source", {{"rootId", "workspace"}, {"relativePath", "mixed-roots.hds"}}},
                              {"output", {{"rootId", "workspace"}, {"relativePath", filename}}},
                              {"roots", {{{"kind", "volume"}, {"partitionIndex", 0U}, {"volumeName", "Mixed"}}}}},
                             context());
        ASSERT_TRUE(exported) << exported.error().message;
    }

    const auto target = images_->open({"workspace", "target.hds"}, "owner");
    ASSERT_TRUE(target) << target.error().message;
    const auto request = nlohmann::json{
        {"imageId", target->image_id},
        {"expectedRevision", target->revision},
        {"packages",
         {{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "shared-one.axkvol"}}}},
          {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "shared-two.axkvol"}}}}}},
        {"destination", {{"kind", "CREATE_VOLUME"}, {"partitionIndex", 0U}, {"volumeName", "Shared"}}},
        {"renames", nlohmann::json::array()},
        {"programSlotAssignments", nlohmann::json::array()},
        {"opaqueSequenceDecisions", nlohmann::json::array()},
    };
    const auto planned = registry_.invoke("images.package_import.plan", request, context());
    ASSERT_TRUE(planned) << planned.error().message;
    ASSERT_TRUE(planned->at("valid").get<bool>());
    ASSERT_EQ(planned->at("packages").size(), 2U);
    EXPECT_EQ(planned->at("packages").at(0).at("destinationVolumeName"), "Shared");
    EXPECT_EQ(planned->at("packages").at(1).at("destinationVolumeName"), "Shared");

    const auto applied = registry_.invoke("images.package_import",
                                          {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    const auto refreshed = images_->inspect(target->image_id, "owner");
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_FALSE(volume_content_id(*refreshed, "Shared").empty());
}

TEST_F(PackageOperationsTest, SessionExportsExactSingleAndMultiRootPackagesToWorkspaceOrRetainedDownload) {
    const auto opened = images_->open({"workspace", "fixture.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto base =
        nlohmann::json{{"imageId", opened->image_id},
                       {"expectedRevision", opened->revision},
                       {"roots", {{{"kind", "VOLUME"}, {"contentId", volume_content_id(*opened, "New Volume")}}}}};

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

TEST_F(PackageOperationsTest, SessionExportsA3kArchiveVolumeAsDirectPackage) {
    const auto opened = images_->open({"workspace", "archive.a3k"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto request = nlohmann::json{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"roots", {{{"kind", "VOLUME"}, {"contentId", volume_content_id(*opened, "Archive Volume")}}}},
        {"destination", {{"kind", "DOWNLOAD"}, {"filename", "archive-volume.axkvol"}}},
    };
    const auto exported = registry_.invoke("images.package_export", request, context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("sourceMediaKind"), "a3k-archive");
    const auto retained = exported->at("download");
    const auto content = downloads_->open({retained.at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(content) << content.error().message;
    const auto package = axk::open_portable_package(*content->reader, content->snapshot.filename);
    ASSERT_TRUE(package) << package.error().message;
    EXPECT_EQ(package->kind, axk::PackageKind::volume);
    EXPECT_EQ(package->source_media_kind, "a3k-archive");
}

TEST_F(PackageOperationsTest, InspectsA3kArchiveAsAnImportableVolumePackage) {
    const auto inspected = registry_.invoke(
        "package.inspect", {{"package", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "archive.a3k"}}}}}},
        context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->at("packageKind"), "volume");
    EXPECT_EQ(inspected->at("requiredExtension"), ".axkvol");
    EXPECT_EQ(inspected->at("sourceMediaKind"), "a3k-archive");
    ASSERT_EQ(inspected->at("roots").size(), 1U);
    EXPECT_EQ(inspected->at("roots").front().at("displayName"), "Archive Volume");
}

TEST_F(PackageOperationsTest, SessionImportCreatesOneExplicitlyNamedVolumeForA3kArchive) {
    const auto opened = images_->open({"workspace", "target.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto request = nlohmann::json{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"packages", {{{"fileRef", {{"rootId", "workspace"}, {"relativePath", "archive.a3k"}}}}}},
        {"destination", {{"kind", "CREATE_VOLUME"}, {"partitionIndex", 0U}, {"volumeName", "Imported A3K"}}},
        {"renames", nlohmann::json::array()},
        {"programSlotAssignments", nlohmann::json::array()},
        {"opaqueSequenceDecisions", nlohmann::json::array()},
    };
    const auto planned = registry_.invoke("images.package_import.plan", request, context());
    ASSERT_TRUE(planned) << planned.error().message;
    ASSERT_TRUE(planned->at("valid").get<bool>());
    ASSERT_EQ(planned->at("packages").size(), 1U);
    EXPECT_EQ(planned->at("packages").front().at("destinationVolumeName"), "Imported A3K");

    const auto applied = registry_.invoke("images.package_import",
                                          {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    const auto refreshed = images_->inspect(opened->image_id, "owner");
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_FALSE(volume_content_id(*refreshed, "Imported A3K").empty());
}

TEST_F(PackageOperationsTest, SessionInspectsAndExportsImmediateVolumePackagesWithReport) {
    const auto opened = images_->open({"workspace", "batch-volumes.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = images_->content(opened->image_id, "owner", 500U);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto scope = std::ranges::find(roots->items, "partition", &axk::app::ImageContentItem::kind);
    ASSERT_NE(scope, roots->items.end());
    const nlohmann::json base{
        {"imageId", opened->image_id}, {"expectedRevision", opened->revision}, {"scopeId", scope->id}};

    const auto inspected = registry_.invoke("images.volume_package_export.inspect", base, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->at("volumeCount"), 3U);
    EXPECT_EQ(inspected->at("exportableCount"), 2U);
    EXPECT_EQ(inspected->at("emptyCount"), 1U);
    ASSERT_EQ(inspected->at("volumes").size(), 3U);
    std::set<std::string> package_paths;
    for (const auto &volume : inspected->at("volumes")) {
        if (volume.at("state") == "READY")
            package_paths.insert(volume.at("packagePath").get<std::string>());
    }
    EXPECT_EQ(package_paths.size(), 2U);

    auto workspace_request = base;
    workspace_request["destination"] = {
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "volume-packages"}}},
    };
    const auto exported = registry_.invoke("images.volume_package_export", workspace_request, context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("exportedCount"), 2U);
    EXPECT_EQ(exported->at("skippedCount"), 1U);
    EXPECT_EQ(exported->at("failedCount"), 0U);
    EXPECT_EQ(exported->at("reportPath"), "volume-packages.axklib.json");
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "volume-packages" / "volume-packages.axklib.json"));
    for (const auto &path : package_paths) {
        const auto package_path = root_ / "volume-packages" / path;
        ASSERT_TRUE(std::filesystem::is_regular_file(package_path));
        const auto package = axk::open_portable_package(package_path);
        ASSERT_TRUE(package) << package.error().message;
        EXPECT_EQ(package->kind, axk::PackageKind::volume);
    }

    auto download_request = base;
    download_request["destination"] = {{"kind", "DOWNLOAD"}, {"directoryName", "local-volume-packages"}};
    const auto download = registry_.invoke("images.volume_package_export", download_request, context());
    ASSERT_TRUE(download) << download.error().message;
    EXPECT_EQ(download->at("destination"), "DOWNLOAD");
    const auto retained = downloads_->open({download->at("download").at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(retained) << retained.error().message;
    EXPECT_EQ(retained->snapshot.entry_count, 3U);
}

TEST_F(PackageOperationsTest, SessionVolumePackageExportPublishesNothingWhenEveryVolumeIsEmpty) {
    const auto opened = images_->open({"workspace", "target.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = images_->content(opened->image_id, "owner", 500U);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto scope = std::ranges::find(roots->items, "partition", &axk::app::ImageContentItem::kind);
    ASSERT_NE(scope, roots->items.end());

    const auto exported = registry_.invoke(
        "images.volume_package_export",
        {{"imageId", opened->image_id},
         {"expectedRevision", opened->revision},
         {"scopeId", scope->id},
         {"destination",
          {{"kind", "WORKSPACE"}, {"output", {{"rootId", "workspace"}, {"relativePath", "empty-volume-packages"}}}}}},
        context());
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, "volume_package_export_empty");
    EXPECT_FALSE(std::filesystem::exists(root_ / "empty-volume-packages"));
}

TEST_F(PackageOperationsTest, SessionVolumePackageExportCancellationPublishesNothing) {
    const auto opened = images_->open({"workspace", "batch-volumes.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = images_->content(opened->image_id, "owner", 500U);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto scope = std::ranges::find(roots->items, "partition", &axk::app::ImageContentItem::kind);
    ASSERT_NE(scope, roots->items.end());

    axk::CancellationSource cancellation;
    cancellation.cancel();
    auto operation_context = context();
    operation_context.cancellation = cancellation.token();
    const auto exported = registry_.invoke(
        "images.volume_package_export",
        {{"imageId", opened->image_id},
         {"expectedRevision", opened->revision},
         {"scopeId", scope->id},
         {"destination",
          {{"kind", "WORKSPACE"}, {"output", {{"rootId", "workspace"}, {"relativePath", "cancelled"}}}}}},
        operation_context);
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, "operation_cancelled");
    EXPECT_FALSE(std::filesystem::exists(root_ / "cancelled"));
}

TEST_F(PackageOperationsTest, SessionInspectsAndExportsSfzToWorkspaceOrRetainedTar) {
    const auto opened = images_->open({"workspace", "fixture.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto sample = std::ranges::find(objects->items, "SBNK", &axk::app::ImageObjectItem::type);
    ASSERT_NE(sample, objects->items.end());
    const nlohmann::json roots{{{"kind", "SBNK"}, {"objectId", sample->id}}};
    const auto base = nlohmann::json{{"imageId", opened->image_id},
                                     {"expectedRevision", opened->revision},
                                     {"selectionMode", "DEPENDENCY_CLOSURE"},
                                     {"roots", roots}};

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

TEST_F(PackageOperationsTest, SessionExportsOnlySelectedSampleAsOneFlatWav) {
    const auto opened = images_->open({"workspace", "fixture.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto sample = std::ranges::find(objects->items, "SBNK", &axk::app::ImageObjectItem::type);
    ASSERT_NE(sample, objects->items.end());
    const nlohmann::json base{{"imageId", opened->image_id},
                              {"expectedRevision", opened->revision},
                              {"selectionMode", "SELECTED_AUDIO_OBJECTS"},
                              {"roots", {{{"kind", "SBNK"}, {"objectId", sample->id}}}}};

    const auto inspected = registry_.invoke("images.audio_export.inspect", base, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->at("wavFileCount"), 1U);
    EXPECT_TRUE(inspected->at("issues").empty());

    auto request = base;
    request["format"] = "WAV";
    request["destination"] = {
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "selected-sample-wav"}}},
    };
    const auto exported = registry_.invoke("images.audio_export", request, context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("fileCount"), 1U);
    const auto output = root_ / "selected-sample-wav";
    ASSERT_TRUE(std::filesystem::is_directory(output));
    std::vector<std::filesystem::directory_entry> wav_files;
    for (const auto &entry : std::filesystem::directory_iterator{output}) {
        if (entry.is_directory()) {
            EXPECT_EQ(entry.path().filename(), ".axklib-publication");
            EXPECT_TRUE(std::filesystem::is_empty(entry.path()));
            continue;
        }
        if (entry.path().extension() == ".wav")
            wav_files.push_back(entry);
    }
    ASSERT_EQ(wav_files.size(), 1U);
    EXPECT_TRUE(wav_files.front().is_regular_file());
}

TEST_F(PackageOperationsTest, SessionExportsSelectedMonoAndStereoSamplesAsOneBatch) {
    const auto opened = images_->open({"workspace", "selected-wav.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto object_id = [&](std::string_view name) {
        const auto found = std::ranges::find_if(
            objects->items, [&](const auto &object) { return object.type == "SBNK" && object.name == name; });
        EXPECT_NE(found, objects->items.end());
        return found == objects->items.end() ? std::string{} : found->id;
    };
    const nlohmann::json base{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"selectionMode", "SELECTED_AUDIO_OBJECTS"},
        {"roots",
         {{{"kind", "SBNK"}, {"objectId", object_id("Mono Sample")}},
          {{"kind", "SBNK"}, {"objectId", object_id("Stereo Sample")}}}},
    };

    const auto inspected = registry_.invoke("images.audio_export.inspect", base, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->at("wavFileCount"), 2U);
    EXPECT_TRUE(inspected->at("issues").empty());

    auto request = base;
    request["format"] = "WAV";
    request["destination"] = {
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "selected-wav-batch"}}},
    };
    const auto exported = registry_.invoke("images.audio_export", request, context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("fileCount"), 2U);
    std::size_t wav_file_count{};
    for (const auto &entry : std::filesystem::directory_iterator{root_ / "selected-wav-batch"}) {
        if (entry.is_regular_file() && entry.path().extension() == ".wav")
            ++wav_file_count;
    }
    EXPECT_EQ(wav_file_count, 2U);
}

TEST_F(PackageOperationsTest, SessionRejectsInvalidSelectedStereoSampleDuringInspection) {
    const auto opened = images_->open(
        {"workspace", "invalid-stereo-objects", axk::app::ImageSourceKind::axk_object_directory}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto sample = std::ranges::find_if(
        objects->items, [](const auto &object) { return object.type == "SBNK" && object.name == "Stereo Sample"; });
    ASSERT_NE(sample, objects->items.end());
    const nlohmann::json request{
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"selectionMode", "SELECTED_AUDIO_OBJECTS"},
        {"roots", {{{"kind", "SBNK"}, {"objectId", sample->id}}}},
    };

    const auto inspected = registry_.invoke("images.audio_export.inspect", request, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    ASSERT_EQ(inspected->at("issues").size(), 1U);
    EXPECT_EQ(inspected->at("issues").front().at("code"), "sample_has_invalid_wave_data_membership");
    EXPECT_TRUE(inspected->at("issues").front().at("fatal").get<bool>());
}

TEST_F(PackageOperationsTest, SessionIgnoresStoredProgramRowsWithoutAnExactTargetDuringSfzExport) {
    const auto source_path = root_ / "warned-audio.hds";
    std::filesystem::copy_file(root_ / "mixed-roots.hds", source_path);
    make_first_program_assignment_context_only(source_path);
    const auto opened = images_->open({"workspace", "warned-audio.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const nlohmann::json roots{{{"kind", "VOLUME"}, {"contentId", volume_content_id(*opened, "Mixed")}}};
    const auto base = nlohmann::json{{"imageId", opened->image_id},
                                     {"expectedRevision", opened->revision},
                                     {"selectionMode", "DEPENDENCY_CLOSURE"},
                                     {"roots", roots}};

    const auto inspected = registry_.invoke("images.audio_export.inspect", base, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_GT(inspected->at("sfzFileCount").get<std::size_t>(), 0U);
    EXPECT_TRUE(inspected->at("issues").empty());
    EXPECT_TRUE(inspected->at("sfzEligible").get<bool>());

    auto request = base;
    request["format"] = "SFZ";
    request["destination"] = {
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "warned-sfz"}}},
    };
    const auto exported = registry_.invoke("images.audio_export", request, context());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->at("format"), "SFZ");
    EXPECT_GT(exported->at("fileCount").get<std::size_t>(), 0U);
}

TEST_F(PackageOperationsTest, SessionRejectsSfzWhenAnySelectionIssueIsFatal) {
    write_audio_source_with_orphan_wave_data(root_ / "fatal-audio.hds");
    const auto opened = images_->open({"workspace", "fatal-audio.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto object_id = [&](std::string_view type, std::string_view name) {
        const auto found = std::ranges::find_if(
            objects->items, [&](const auto &object) { return object.type == type && object.name == name; });
        EXPECT_NE(found, objects->items.end());
        return found == objects->items.end() ? std::string{} : found->id;
    };
    const nlohmann::json roots{
        {{"kind", "SBNK"}, {"objectId", object_id("SBNK", "Linked Sample")}},
        {{"kind", "SMPL"}, {"objectId", object_id("SMPL", "Orphan Wave")}},
    };
    const auto base = nlohmann::json{{"imageId", opened->image_id},
                                     {"expectedRevision", opened->revision},
                                     {"selectionMode", "DEPENDENCY_CLOSURE"},
                                     {"roots", roots}};

    const auto inspected = registry_.invoke("images.audio_export.inspect", base, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_GT(inspected->at("sfzFileCount").get<std::size_t>(), 0U);
    ASSERT_EQ(inspected->at("issues").size(), 1U);
    EXPECT_EQ(inspected->at("issues").at(0).at("code"), "wave_data_has_no_confirmed_sample");
    EXPECT_TRUE(inspected->at("issues").at(0).at("fatal").get<bool>());
    EXPECT_FALSE(inspected->at("sfzEligible").get<bool>());

    auto request = base;
    request["format"] = "SFZ";
    request["destination"] = {{"kind", "DOWNLOAD"}, {"directoryName", "Rejected SFZ"}};
    const auto exported = registry_.invoke("images.audio_export", request, context());
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, "sfz_semantics_unavailable");
}

TEST_F(PackageOperationsTest, SessionExportsSequencesAsMidiToWorkspaceOrRetainedTar) {
    const auto opened = images_->open({"workspace", "sequence.bin", axk::app::ImageSourceKind::file}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    const auto sequence = std::ranges::find(objects->items, "SEQU", &axk::app::ImageObjectItem::type);
    ASSERT_NE(sequence, objects->items.end());
    ASSERT_TRUE(sequence->sequence);
    EXPECT_EQ(sequence->sequence->ticks_per_quarter_note, 96U);
    EXPECT_GT(sequence->sequence->event_count, 0U);
    EXPECT_EQ(sequence->sequence->header_tempo_bpm, 120U);
    EXPECT_EQ(sequence->sequence->effective_initial_tempo_microseconds_per_quarter_note, 500'000U);
    EXPECT_TRUE(sequence->sequence->tempo_events.empty());

    const nlohmann::json base{
        {"imageId", opened->image_id}, {"expectedRevision", opened->revision}, {"objectIds", {sequence->id}}};
    auto workspace_request = base;
    workspace_request["destination"] = {
        {"kind", "WORKSPACE"},
        {"output", {{"rootId", "workspace"}, {"relativePath", "midi-workspace"}}},
    };
    const auto workspace = registry_.invoke("images.sequence_export", workspace_request, context());
    ASSERT_TRUE(workspace) << workspace.error().message;
    EXPECT_EQ(workspace->at("destination"), "WORKSPACE");
    EXPECT_EQ(workspace->at("sequenceCount"), 1U);
    const auto midi_path = root_ / "midi-workspace" / "Sequence.mid";
    ASSERT_TRUE(std::filesystem::is_regular_file(midi_path));
    const auto encoded =
        axk::smf0_to_current_sequence(read_bytes(midi_path), "Roundtrip", axk::SequenceSystemExclusivePolicy::reject);
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto decoded = axk::decode_current_sequence(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->event_count, sequence->sequence->event_count + 1U);
    ASSERT_EQ(decoded->tempo_events.size(), 1U);
    EXPECT_EQ(decoded->tempo_events.front().tick, 0U);
    EXPECT_EQ(decoded->tempo_events.front().microseconds_per_quarter_note, 500'000U);

    auto download_request = base;
    download_request["destination"] = {{"kind", "DOWNLOAD"}, {"directoryName", "Local MIDI"}};
    const auto download = registry_.invoke("images.sequence_export", download_request, context());
    ASSERT_TRUE(download) << download.error().message;
    ASSERT_EQ(download->at("destination"), "DOWNLOAD");
    const auto &retained = download->at("download");
    ASSERT_EQ(retained.at("filename"), "Local MIDI.tar");
    const auto content = downloads_->open({retained.at("archiveId").get<std::string>()}, "owner");
    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(content->snapshot.media_type, "application/x-tar");
    EXPECT_EQ(content->snapshot.entry_count, 1U);
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

    const auto exported =
        registry_.invoke("images.package_export",
                         {{"imageId", opened->image_id},
                          {"expectedRevision", opened->revision},
                          {"roots", {{{"kind", "VOLUME"}, {"contentId", volume.id}}}},
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

TEST_F(PackageOperationsTest, SessionExportsFlatMediaAudioIntoOneSharedPoolWithoutSyntheticHierarchy) {
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
        "images.audio_export",
        {{"imageId", opened->image_id},
         {"expectedRevision", opened->revision},
         {"selectionMode", "DEPENDENCY_CLOSURE"},
         {"roots", {{{"kind", "VOLUME"}, {"contentId", volume.id}}}},
         {"format", "WAV"},
         {"destination",
          {{"kind", "WORKSPACE"}, {"output", {{"rootId", "workspace"}, {"relativePath", "object-directory-audio"}}}}}},
        context());
    ASSERT_TRUE(exported) << exported.error().message;

    const auto output = root_ / "object-directory-audio";
    EXPECT_TRUE(std::filesystem::is_directory(output / "_samples" / "physical"));
    for (const auto &entry : std::filesystem::recursive_directory_iterator{output}) {
        const auto relative = std::filesystem::relative(entry.path(), output);
        EXPECT_NE(relative.begin()->string(), "objects");
        EXPECT_NE(relative.begin()->string(), "Object directory");
    }
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
    const nlohmann::json roots{{{"kind", "VOLUME"}, {"contentId", volume.id}}};
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

    const auto attached = images_->attach_companions(
        opened->image_id, "owner", opened->revision,
        {axk::app::CompanionSelectionKind::sources,
         {{"workspace", "disk-set/DISK1", axk::app::ImageSourceKind::axk_object_directory}}});
    ASSERT_TRUE(attached) << attached.error().message;
    const nlohmann::json attached_roots{{{"kind", "VOLUME"}, {"contentId", volume_content_id(*attached, volume.name)}}};
    const auto exported = registry_.invoke("images.package_export",
                                           {{"imageId", attached->image_id},
                                            {"expectedRevision", attached->revision},
                                            {"roots", attached_roots},
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

TEST_F(PackageOperationsTest, ExportsSequenceRootsAndRejectsUploadOwnershipMismatch) {
    const auto sequence = registry_.invoke("package.export",
                                           {{"source", {{"rootId", "workspace"}, {"relativePath", "sequence.bin"}}},
                                            {"output", {{"rootId", "workspace"}, {"relativePath", "sequence.axkseq"}}},
                                            {"roots", {{{"kind", "sequence"}, {"objectName", "Sequence"}}}}},
                                           context());
    ASSERT_TRUE(sequence) << sequence.error().message;
    EXPECT_EQ(sequence->at("packageKind"), "sequence");
    EXPECT_EQ(sequence->at("requiredExtension"), ".axkseq");

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
