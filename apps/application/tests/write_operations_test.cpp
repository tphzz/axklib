#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/application/session_extent_layout_repair_operations.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/sfs.hpp"
#include "axklib/sfs_repair.hpp"
#include "axklib/writer.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

nlohmann::json file_ref(std::string relative_path) {
    return {{"rootId", "workspace"}, {"relativePath", std::move(relative_path)}};
}

void write_tone(const std::filesystem::path &path) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44'100};
    waveform.frame_count = 3;
    waveform.pcm = {std::byte{}, std::byte{}, std::byte{0xe8}, std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
    ASSERT_TRUE(axk::write_wav_atomic(path, waveform));
}

axk::HdsBuildManifest placement_repair_manifest(const std::filesystem::path &audio_path) {
    axk::VolumeSpec volume;
    volume.name = "Samples";
    volume.waveforms.push_back({"wave", "Wave", audio_path, 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Old Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60U;
    sample.key_high = 127U;
    volume.samples.push_back(std::move(sample));
    return {"1.0", 4U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};
}

void patch_index_be32(const std::filesystem::path &path, const axk::IndexRecord &record, std::size_t offset,
                      std::uint32_t value) {
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const std::array bytes{static_cast<char>((value >> 24U) & 0xffU), static_cast<char>((value >> 16U) & 0xffU),
                           static_cast<char>((value >> 8U) & 0xffU), static_cast<char>(value & 0xffU)};
    image.seekp(static_cast<std::streamoff>(record.record_offset.value + offset));
    image.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(image);
}

std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> result;
    result.reserve(bytes.size());
    for (const auto byte : bytes)
        result.push_back(static_cast<std::byte>(byte));
    return result;
}

class MutateSourceOnCompletedWrite final : public axk::ProgressSink {
  public:
    explicit MutateSourceOnCompletedWrite(std::filesystem::path source) : source_{std::move(source)} {}

    void report(const axk::Progress &progress) noexcept override {
        if (mutated_ || progress.phase != axk::ProgressPhase::writing || !progress.total ||
            progress.completed != *progress.total) {
            return;
        }
        std::ofstream{source_, std::ios::binary | std::ios::app} << "changed during apply";
        mutated_ = true;
    }

  private:
    std::filesystem::path source_;
    bool mutated_{};
};

class CancelOnPhase final : public axk::ProgressSink {
  public:
    CancelOnPhase(axk::CancellationSource &cancellation, axk::ProgressPhase phase)
        : cancellation_{cancellation}, phase_{phase} {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == phase_)
            cancellation_.cancel();
    }

  private:
    axk::CancellationSource &cancellation_;
    axk::ProgressPhase phase_;
};

bool has_publication_temporary(const std::filesystem::path &root) {
    for (const auto &entry : std::filesystem::directory_iterator{root}) {
        if (entry.path().filename().string().find(".axklib-publication.") != std::string::npos)
            return true;
    }
    return false;
}

axk::HdsBuildManifest all_action_source_manifest(const std::filesystem::path &audio_path) {
    axk::VolumeSpec volume;
    volume.name = "Volume";
    volume.waveforms = {
        {"wave", "Wave", audio_path, 60U, {}},
        {"delete-wave", "Delete Wave", audio_path, 60U, {}},
        {"old-wave", "Old Wave", audio_path, 60U, {}},
    };
    for (const auto *name : {"Delete Sample", "Old Sample", "Sample A", "Sample B", "Del Bank Sample", "Delete Direct",
                             "Old Bank Sample", "Old Direct"}) {
        axk::SampleSpec sample;
        sample.name = name;
        sample.waveform_id = "wave";
        sample.root_key = 60U;
        sample.key_high = 127U;
        volume.samples.push_back(std::move(sample));
    }
    volume.sample_banks = {
        {"Delete Bank", {"Del Bank Sample"}},
        {"Old Bank", {"Old Bank Sample"}},
    };
    volume.programs = {
        {128U, "Pgm 128", {{"SBAC", "Delete Bank", 1U}, {"SBNK", "Delete Direct", 2U}}},
        {127U, "Pgm 127", {{"SBAC", "Old Bank", 1U}, {"SBNK", "Old Direct", 2U}}},
    };

    axk::VolumeSpec deleted_volume;
    deleted_volume.name = "Delete Volume";
    return {"1.0", 8U * 1024U * 1024U, {{"hd1", {std::move(volume), std::move(deleted_volume)}}}};
}

nlohmann::json all_action_alteration_manifest() {
    return {
        {"schema_version", "1.0"},
        {"operations",
         nlohmann::json::array({
             {{"id", "delete-volume"},
              {"type", "delete_volume"},
              {"partition_index", 0U},
              {"volume_name", "Delete Volume"}},
             {{"id", "insert-volume"},
              {"type", "insert_volume"},
              {"partition_index", {{"operation_ref", "delete-volume"}}},
              {"volume",
               {{"name", "Insert Volume"},
                {"waveforms", nlohmann::json::array()},
                {"samples", nlohmann::json::array()}}}},
             {{"id", "delete-sample"},
              {"type", "delete_sbnk"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"sample_name", "Delete Sample"}},
             {{"id", "insert-sample"},
              {"type", "insert_sbnk"},
              {"partition_index", {{"operation_ref", "delete-sample"}}},
              {"volume_name", "Volume"},
              {"sample",
               {{"name", "Insert Sample"},
                {"waveform_name", "Wave"},
                {"root_key", 60U},
                {"key_low", 0U},
                {"key_high", 127U}}}},
             {{"id", "insert-wave"},
              {"type", "insert_waveform"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"audio", {{"path", "insert.wav"}, {"waveform_names", {"Insert Wave"}}, {"root_key", 60U}}}},
             {{"id", "delete-wave"},
              {"type", "delete_waveform"},
              {"partition_index", {{"operation_ref", "insert-wave"}}},
              {"volume_name", "Volume"},
              {"waveform_name", "Delete Wave"}},
             {{"id", "rename-wave"},
              {"type", "rename_waveform"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"waveform_name", "Old Wave"},
              {"new_waveform_name", "New Wave"}},
             {{"id", "rename-sample"},
              {"type", "rename_sbnk"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"sample_name", "Old Sample"},
              {"new_sample_name", "New Sample"}},
             {{"id", "delete-program"},
              {"type", "delete_program"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"program_number", 128U}},
             {{"id", "delete-sample-sample"},
              {"type", "delete_sbac"},
              {"partition_index", {{"operation_ref", "delete-program"}}},
              {"volume_name", "Volume"},
              {"sample_bank_name", "Delete Bank"}},
             {{"id", "insert-sample-sample"},
              {"type", "insert_sbac"},
              {"partition_index", {{"operation_ref", "delete-sample-sample"}}},
              {"volume_name", "Volume"},
              {"sample_bank", {{"name", "Insert Bank"}, {"member_samples", {"Sample A", "Sample B"}}}}},
             {{"id", "rename-sample-sample"},
              {"type", "rename_sbac"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"sample_bank_name", "Old Bank"},
              {"new_sample_bank_name", "New Bank"}},
             {{"id", "insert-program"},
              {"type", "insert_program"},
              {"partition_index", {{"operation_ref", "rename-sample-sample"}}},
              {"volume_name", "Volume"},
              {"program",
               {{"number", 128U},
                {"name", "Inserted"},
                {"assignments",
                 {{{"sample_bank", "Insert Bank"}, {"receive_mode", "MIDI_CHANNEL"}, {"receive_channel", 1U}},
                  {{"sample", "Delete Direct"}, {"receive_mode", "MIDI_CHANNEL"}, {"receive_channel", 2U}}}}}}},
             {{"id", "rename-program"},
              {"type", "rename_program"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"program_number", 127U},
              {"new_program_name", "Renamed"}},
         })},
    };
}

class WriteOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-write-operations-test";
        now_ = std::chrono::steady_clock::now();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
        std::filesystem::copy_file(fixture_path(), root_ / "fixture.hds");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox);
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        uploads_ = std::make_unique<axk::app::UploadStore>(root_ / "uploads", 16U * 1024U * 1024U, 8U * 1024U * 1024U,
                                                           8U, 2U * 1024U * 1024U, std::chrono::seconds{5},
                                                           [this] { return now_; });
        downloads_ = std::make_unique<axk::app::DownloadArchiveStore>(root_ / "downloads", 16U * 1024U * 1024U,
                                                                      8U * 1024U * 1024U, 8U, std::chrono::seconds{5},
                                                                      [this] { return now_; });
        registry_ = axk::app::make_operation_registry();
        ASSERT_TRUE(axk::app::bind_write_operations(registry_, *sandbox_, *uploads_));
        reservations_ = std::make_unique<axk::app::PathReservationCoordinator>();
        images_ = std::make_unique<axk::app::ImageSessionManager>(*sandbox_, 4U, 500U, std::chrono::minutes{15},
                                                                  std::chrono::steady_clock::now, reservations_.get());
        journals_ = std::make_unique<axk::app::AlterationJournalStore>(root_ / "journals");
        ASSERT_TRUE(journals_->storage_ready());
        ASSERT_TRUE(axk::app::bind_session_write_operations(registry_, *sandbox_, *uploads_, *images_, *journals_));
        ASSERT_TRUE(
            axk::app::bind_session_extent_layout_repair_operations(registry_, *sandbox_, *images_, *downloads_));
    }

    void TearDown() override {
        images_.reset();
        journals_.reset();
        reservations_.reset();
        downloads_.reset();
        uploads_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    axk::app::OperationContext context() const {
        return {
            .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    }

    std::filesystem::path root_;
    std::chrono::steady_clock::time_point now_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    std::unique_ptr<axk::app::UploadStore> uploads_;
    std::unique_ptr<axk::app::DownloadArchiveStore> downloads_;
    std::unique_ptr<axk::app::PathReservationCoordinator> reservations_;
    std::unique_ptr<axk::app::ImageSessionManager> images_;
    std::unique_ptr<axk::app::AlterationJournalStore> journals_;
    axk::app::OperationRegistry registry_;
};

TEST_F(WriteOperationsTest, SessionExtentLayoutRepairPublishesAValidatedCopyWithoutChangingTheSource) {
    write_tone(root_ / "extent-repair.wav");
    const auto source = root_ / "extent-repair.hds";
    ASSERT_TRUE(axk::write_hds_image(placement_repair_manifest(root_ / "extent-repair.wav"), source));
    auto container = axk::open_image(source);
    ASSERT_TRUE(container) << container.error().message;
    const auto volume_root = std::ranges::find_if(container->partitions().front().records, [](const auto &record) {
        return record.payload_kind == axk::PayloadKind::directory && record.extents.size() == 1U &&
               std::ranges::contains(record.directory_entries, "Samples", &axk::DirectoryEntry::name);
    });
    ASSERT_NE(volume_root, container->partitions().front().records.end());
    ASSERT_LT(volume_root->data_size, volume_root->extents.front().cluster_count * axk::sfs_default_sector_size *
                                          container->partitions().front().sectors_per_cluster);
    patch_index_be32(source, *volume_root, 0x12U, volume_root->extents.front().byte_count + 1U);
    const auto source_bytes = read_bytes(source);

    const auto opened = images_->open({"workspace", "extent-repair.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto repaired =
        registry_.invoke("images.extent_layout.repair",
                         {{"imageId", opened->image_id},
                          {"expectedRevision", opened->revision},
                          {"destination",
                           {{"kind", "WORKSPACE"},
                            {"output", {{"rootId", "workspace"}, {"relativePath", "extent-repaired.hds"}}},
                            {"overwrite", false}}}},
                         context());
    ASSERT_TRUE(repaired) << repaired.error().message;
    EXPECT_EQ(repaired->at("imageId"), opened->image_id);
    EXPECT_EQ(repaired->at("revision"), opened->revision);
    EXPECT_EQ(repaired->at("destination"), "WORKSPACE");
    EXPECT_EQ(repaired->at("output").at("relativePath"), "extent-repaired.hds");
    ASSERT_EQ(repaired->at("repairs").size(), 1U);
    const auto &repair = repaired->at("repairs").front();
    EXPECT_EQ(repair.at("partitionIndex"), 0U);
    EXPECT_EQ(repair.at("recordId"), volume_root->sfs_id.value);
    EXPECT_EQ(repair.at("logicalSize"), volume_root->data_size);
    ASSERT_EQ(repair.at("sourceExtents").size(), 1U);
    ASSERT_EQ(repair.at("replacementExtents").size(), 1U);
    EXPECT_EQ(repair.at("sourceExtents").front().at("byteCount"), volume_root->extents.front().byte_count + 1U);
    EXPECT_EQ(repair.at("replacementExtents").front().at("byteCount"), volume_root->data_size);

    EXPECT_EQ(read_bytes(source), source_bytes);
    const auto repaired_container = axk::open_image(root_ / "extent-repaired.hds");
    ASSERT_TRUE(repaired_container) << repaired_container.error().message;
    EXPECT_TRUE(std::ranges::all_of(repaired_container->partitions(), [](const auto &partition) {
        return axk::allocation_is_safe_for_mutation(partition.allocation);
    }));
    EXPECT_FALSE(axk::inspect_sfs_extent_layout_repair(*repaired_container));
}

TEST_F(WriteOperationsTest, StarterManifestsAreInlineAndHdsStarterBuildsPersistentValidatedImage) {
    const auto starter = registry_.invoke("create.manifest", {{"kind", "HDS"}}, context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["size_bytes"] = 1'048'576U;
    const auto planned = registry_.invoke("create.plan",
                                          {{"kind", "HDS"},
                                           {"manifest", {{"inline", manifest}}},
                                           {"output", {{"rootId", "workspace"}, {"relativePath", "created.hds"}}}},
                                          context());
    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_EQ(planned->at("kind"), "HDS");
    EXPECT_EQ(planned->at("summary").at("sizeBytes"), 1'048'576U);
    const auto accesses =
        registry_.path_accesses("create.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(accesses) << accesses.error().message;
    ASSERT_EQ(accesses->size(), 1U);
    EXPECT_EQ(accesses->front().reference, (axk::app::FileRef{"workspace", "created.hds"}));
    EXPECT_EQ(accesses->front().mode, axk::app::PathAccessMode::exclusive);

    const auto raw_apply = registry_.invoke(
        "create.hds",
        {{"manifest", {{"inline", manifest}}}, {"output", {{"rootId", "workspace"}, {"relativePath", "created.hds"}}}},
        context());
    ASSERT_FALSE(raw_apply);
    EXPECT_EQ(raw_apply.error().code, "invalid_request");

    const auto built =
        registry_.invoke("create.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "created.hds"));
    EXPECT_EQ(built->at("sizeBytes"), 1'048'576U);
    EXPECT_EQ(built->at("sha256").get<std::string>().size(), 64U);
    EXPECT_TRUE(built->at("validation").at("valid").get<bool>());
    EXPECT_FALSE(
        registry_.invoke("create.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, context()));
}

TEST_F(WriteOperationsTest, PublishesTypedHardDiskCreationProfiles) {
    const auto result = registry_.invoke("create.hds.profiles", nlohmann::json::object(), context());
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("schemaVersion"), "1.0");
    const auto &profiles = result->at("profiles");
    ASSERT_EQ(profiles.size(), 5U);
    EXPECT_EQ(profiles[0].at("profileId"), "FLOPPY_SCALE");
    EXPECT_EQ(profiles[0].at("sizeBytes"), 1'474'560U);
    EXPECT_EQ(profiles[0].at("defaultPartitionCount"), 1U);
    EXPECT_EQ(profiles[0].at("partitionOptions").size(), 1U);
    EXPECT_EQ(profiles[4].at("profileId"), "HDS_2_GIB");
    EXPECT_EQ(profiles[4].at("defaultPartitionCount"), 2U);
    EXPECT_EQ(profiles[4].at("partitionOptions").size(), 7U);
}

TEST_F(WriteOperationsTest, TypedHardDiskPlanUsesTheExistingAtomicBuildLifecycle) {
    const auto planned = registry_.invoke(
        "create.hds.plan", {{"profileId", "FLOPPY_SCALE"}, {"partitionCount", 1U}, {"output", file_ref("quick.hds")}},
        context());
    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_EQ(planned->at("kind"), "HDS");
    EXPECT_EQ(planned->at("summary").at("sizeBytes"), 1'474'560U);
    EXPECT_EQ(planned->at("summary").at("partitionCount"), 1U);

    const auto applied =
        registry_.invoke("create.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "quick.hds"));
    const auto reopened = axk::open_image(root_ / "quick.hds");
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened->partitions().size(), 1U);
    EXPECT_EQ(reopened->partitions()[0].name, "PARTITION 1");

    const auto rejected = registry_.invoke(
        "create.hds.plan", {{"profileId", "HDS_2_GIB"}, {"partitionCount", 1U}, {"output", file_ref("wasteful.hds")}},
        context());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "image_operation_failed");
    EXPECT_FALSE(std::filesystem::exists(root_ / "wasteful.hds"));
}

TEST_F(WriteOperationsTest, TypedFloppyPlanCreatesAnExactSamplerFullFormatImage) {
    const auto planned = registry_.invoke("create.floppy.plan", {{"output", file_ref("blank.ima")}}, context());
    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_EQ(planned->at("kind"), "FLOPPY");
    EXPECT_EQ(planned->at("summary").at("format"), "FLOPPY");
    EXPECT_EQ(planned->at("summary").at("sizeBytes"), 1'474'560U);
    EXPECT_EQ(planned->at("summary").at("objectCount"), 0U);

    const auto built =
        registry_.invoke("create.floppy", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_EQ(built->at("sha256"), "59225775ad66b28498940e34d318826eddd72642214a6655c71d825a5b9c4af5");
    EXPECT_TRUE(built->at("validation").at("valid").get<bool>());
    const auto reopened = axk::open_media(root_ / "blank.ima");
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(std::holds_alternative<axk::FatImage>(reopened->storage()));
}

TEST_F(WriteOperationsTest, TypedHardDiskPlanRemainsValidAfterRegistryMove) {
    auto moved_registry = std::move(registry_);
    const auto planned = moved_registry.invoke(
        "create.hds.plan", {{"profileId", "CD_R_700"}, {"partitionCount", 1U}, {"output", file_ref("moved.hds")}},
        context());

    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_EQ(planned->at("summary").at("sizeBytes"), 737'280'000U);
    EXPECT_EQ(planned->at("summary").at("partitionCount"), 1U);
}

TEST_F(WriteOperationsTest, StarterManifestsDescribeAndBuildEverySupportedImageKind) {
    write_tone(root_ / "tone.wav");

    for (const auto *kind : {"HDS", "FLOPPY", "ISO"}) {
        const auto starter = registry_.invoke("create.manifest", {{"kind", kind}}, context());
        ASSERT_TRUE(starter) << kind << ": " << starter.error().message;
        EXPECT_EQ(starter->at("schemaVersion"), "1.0");
        EXPECT_EQ(nlohmann::json::parse(starter->at("canonicalJson").get<std::string>()), starter->at("manifest"));
        EXPECT_TRUE(starter->at("documentation").get<std::string>().starts_with('/'));
        EXPECT_TRUE(starter->at("choices").at("manifestSources").is_array());
        EXPECT_TRUE(starter->at("choices").at("inputBindingSources").is_array());
        EXPECT_TRUE(starter->at("choices").at("modes").is_array());
        EXPECT_EQ(starter->dump().find(root_.string()), std::string::npos);

        auto manifest = starter->at("manifest");
        if (std::string_view{kind} == "HDS")
            manifest["size_bytes"] = 1'048'576U;
        nlohmann::json request{{"kind", kind},
                               {"manifest", {{"inline", manifest}}},
                               {"output", file_ref(std::string{"built-"} + kind +
                                                   (std::string_view{kind} == "HDS"      ? ".hds"
                                                    : std::string_view{kind} == "FLOPPY" ? ".ima"
                                                                                         : ".iso"))}};
        if (std::string_view{kind} == "FLOPPY") {
            request["inputBindings"] =
                nlohmann::json::array({{{"manifestPath", "tone.wav"}, {"input", {{"fileRef", file_ref("tone.wav")}}}}});
        }
        const auto planned = registry_.invoke("create.plan", request, context());
        ASSERT_TRUE(planned) << kind << ": " << planned.error().message;
        EXPECT_EQ(planned->at("kind"), kind);
        EXPECT_EQ(planned->at("summary").at("format"), kind);
        const auto token = planned->at("planToken").get<std::string>();
        EXPECT_EQ(token.size(), 48U);

        const auto operation = std::string_view{kind} == "HDS"      ? "create.hds"
                               : std::string_view{kind} == "FLOPPY" ? "create.floppy"
                                                                    : "create.iso";
        const auto built = registry_.invoke(operation, {{"planToken", token}}, context());
        ASSERT_TRUE(built) << kind << ": " << built.error().message;
        EXPECT_EQ(built->at("schemaVersion"), "1.0");
        EXPECT_EQ(built->at("kind"), kind);
        EXPECT_EQ(built->at("summary"), planned->at("summary"));
        EXPECT_TRUE(built->at("validation").at("valid").get<bool>());
        EXPECT_TRUE(
            std::filesystem::is_regular_file(root_ / built->at("output").at("relativePath").get<std::string>()));
    }
}

TEST_F(WriteOperationsTest, ManifestInputsAndAudioBindingsAcceptFilesAndUploads) {
    write_tone(root_ / "tone.wav");
    const auto starter = registry_.invoke("create.manifest", {{"kind", "FLOPPY"}}, context());
    ASSERT_TRUE(starter) << starter.error().message;
    const auto manifest_text = starter->at("canonicalJson").get<std::string>();
    std::ofstream{root_ / "floppy.json", std::ios::binary} << manifest_text;

    const auto tone_bytes = read_bytes(root_ / "tone.wav");
    auto audio = uploads_->create({.owner_id = "owner",
                                   .filename = "tone.wav",
                                   .kind = axk::app::UploadKind::audio,
                                   .media_type = "audio/wav",
                                   .declared_size = tone_bytes.size(),
                                   .sha256 = std::nullopt});
    ASSERT_TRUE(audio) << audio.error().message;
    ASSERT_TRUE(uploads_->append(audio->reference, "owner", 0U, tone_bytes));
    ASSERT_TRUE(uploads_->complete(audio->reference, "owner"));

    const auto request = nlohmann::json{
        {"kind", "FLOPPY"},
        {"manifest", {{"fileRef", file_ref("floppy.json")}}},
        {"inputBindings",
         {{{"manifestPath", "tone.wav"}, {"input", {{"uploadRef", {{"uploadId", audio->reference.upload_id}}}}}}}},
        {"output", file_ref("uploaded-audio.ima")},
    };
    const auto planned = registry_.invoke("create.plan", request, context());
    ASSERT_TRUE(planned) << planned.error().message;
    const auto built =
        registry_.invoke("create.floppy", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "uploaded-audio.ima"));

    auto manifest_upload = uploads_->create({.owner_id = "owner",
                                             .filename = "iso.json",
                                             .kind = axk::app::UploadKind::manifest,
                                             .media_type = "application/json",
                                             .declared_size = manifest_text.size(),
                                             .sha256 = std::nullopt});
    ASSERT_TRUE(manifest_upload) << manifest_upload.error().message;
    const auto manifest_bytes = std::as_bytes(std::span{manifest_text});
    ASSERT_TRUE(uploads_->append(manifest_upload->reference, "owner", 0U, manifest_bytes));
    ASSERT_TRUE(uploads_->complete(manifest_upload->reference, "owner"));

    auto upload_request = request;
    upload_request["manifest"] = {{"uploadRef", {{"uploadId", manifest_upload->reference.upload_id}}}};
    upload_request["output"] = file_ref("uploaded-manifest.ima");
    const auto upload_plan = registry_.invoke("create.plan", upload_request, context());
    ASSERT_TRUE(upload_plan) << upload_plan.error().message;
}

TEST_F(WriteOperationsTest, WholeSourceTransferRequiresPersistentFileRef) {
    write_tone(root_ / "tone.wav");
    const auto floppy_starter = registry_.invoke("create.manifest", {{"kind", "FLOPPY"}}, context());
    ASSERT_TRUE(floppy_starter) << floppy_starter.error().message;
    const auto floppy_plan = registry_.invoke(
        "create.plan",
        {{"kind", "FLOPPY"},
         {"manifest", {{"inline", floppy_starter->at("manifest")}}},
         {"inputBindings", {{{"manifestPath", "tone.wav"}, {"input", {{"fileRef", file_ref("tone.wav")}}}}}},
         {"output", file_ref("source.ima")}},
        context());
    ASSERT_TRUE(floppy_plan) << floppy_plan.error().message;
    ASSERT_TRUE(
        registry_.invoke("create.floppy", {{"planToken", floppy_plan->at("planToken").get<std::string>()}}, context()));

    const nlohmann::json manifest = {
        {"schema_version", "1.0"},
        {"format", "iso9660"},
        {"iso",
         {{"volume_id", "AXK_TEST"},
          {"raw_group", "00000010"},
          {"group_name", "Test Group"},
          {"raw_volume", "F001"},
          {"volume_name", "Test Volume"}}},
        {"transfer", {{"source_path", "source.ima"}, {"selection", "all"}}},
    };
    const auto source_bytes = read_bytes(root_ / "source.ima");
    auto uploaded_source = uploads_->create({.owner_id = "owner",
                                             .filename = "source.axkvol",
                                             .kind = axk::app::UploadKind::package,
                                             .media_type = "application/octet-stream",
                                             .declared_size = source_bytes.size(),
                                             .sha256 = std::nullopt});
    ASSERT_TRUE(uploaded_source) << uploaded_source.error().message;
    ASSERT_TRUE(uploads_->append(uploaded_source->reference, "owner", 0U, source_bytes));
    ASSERT_TRUE(uploads_->complete(uploaded_source->reference, "owner"));

    const auto upload_plan =
        registry_.invoke("create.plan",
                         {{"kind", "ISO"},
                          {"manifest", {{"inline", manifest}}},
                          {"inputBindings",
                           {{{"manifestPath", "source.ima"},
                             {"input", {{"uploadRef", {{"uploadId", uploaded_source->reference.upload_id}}}}}}}},
                          {"output", file_ref("upload-transfer.iso")}},
                         context());
    ASSERT_FALSE(upload_plan);
    EXPECT_EQ(upload_plan.error().code, "whole_source_requires_file_ref");

    const auto file_plan = registry_.invoke(
        "create.plan",
        {{"kind", "ISO"},
         {"manifest", {{"inline", manifest}}},
         {"inputBindings", {{{"manifestPath", "source.ima"}, {"input", {{"fileRef", file_ref("source.ima")}}}}}},
         {"output", file_ref("file-transfer.iso")}},
        context());
    ASSERT_TRUE(file_plan) << file_plan.error().message;
}

TEST_F(WriteOperationsTest, BuildPlanningRejectsInvalidBindingsAndExpiredUploads) {
    write_tone(root_ / "tone.wav");
    const auto starter = registry_.invoke("create.manifest", {{"kind", "FLOPPY"}}, context());
    ASSERT_TRUE(starter) << starter.error().message;
    const nlohmann::json binding = {{"manifestPath", "tone.wav"}, {"input", {{"fileRef", file_ref("tone.wav")}}}};
    const nlohmann::json base_request = {
        {"kind", "FLOPPY"},
        {"manifest", {{"inline", starter->at("manifest")}}},
        {"output", file_ref("invalid-bindings.ima")},
    };

    auto duplicate = base_request;
    duplicate["inputBindings"] = nlohmann::json::array({binding, binding});
    const auto duplicate_plan = registry_.invoke("create.plan", duplicate, context());
    ASSERT_FALSE(duplicate_plan);
    EXPECT_EQ(duplicate_plan.error().code, "invalid_binding_path");

    auto traversal = base_request;
    traversal["inputBindings"] =
        nlohmann::json::array({{{"manifestPath", "../tone.wav"}, {"input", {{"fileRef", file_ref("tone.wav")}}}}});
    const auto traversal_plan = registry_.invoke("create.plan", traversal, context());
    ASSERT_FALSE(traversal_plan);
    EXPECT_EQ(traversal_plan.error().code, "invalid_binding_path");

    const auto tone_bytes = read_bytes(root_ / "tone.wav");
    auto upload = uploads_->create({.owner_id = "owner",
                                    .filename = "tone.wav",
                                    .kind = axk::app::UploadKind::audio,
                                    .media_type = "audio/wav",
                                    .declared_size = tone_bytes.size(),
                                    .sha256 = std::nullopt});
    ASSERT_TRUE(upload) << upload.error().message;
    ASSERT_TRUE(uploads_->append(upload->reference, "owner", 0U, tone_bytes));
    ASSERT_TRUE(uploads_->complete(upload->reference, "owner"));
    now_ += std::chrono::seconds{6};
    uploads_->cleanup();

    auto expired = base_request;
    expired["inputBindings"] = nlohmann::json::array(
        {{{"manifestPath", "tone.wav"}, {"input", {{"uploadRef", {{"uploadId", upload->reference.upload_id}}}}}}});
    const auto expired_plan = registry_.invoke("create.plan", expired, context());
    ASSERT_FALSE(expired_plan);
    EXPECT_EQ(expired_plan.error().code, "upload_not_found");
}

TEST_F(WriteOperationsTest, BuildPlansAreOwnerBoundAndRejectChangedInputs) {
    write_tone(root_ / "tone.wav");
    const auto starter = registry_.invoke("create.manifest", {{"kind", "FLOPPY"}}, context());
    ASSERT_TRUE(starter) << starter.error().message;
    const auto planned = registry_.invoke(
        "create.plan",
        {{"kind", "FLOPPY"},
         {"manifest", {{"inline", starter->at("manifest")}}},
         {"inputBindings", {{{"manifestPath", "tone.wav"}, {"input", {{"fileRef", file_ref("tone.wav")}}}}}},
         {"output", file_ref("stale-input.ima")}},
        context());
    ASSERT_TRUE(planned) << planned.error().message;
    const auto token = planned->at("planToken").get<std::string>();

    auto other = context();
    other.owner_id = "other";
    const auto denied = registry_.invoke("create.floppy", {{"planToken", token}}, other);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "write_plan_not_found");

    std::ofstream{root_ / "tone.wav", std::ios::binary | std::ios::app} << "changed";
    const auto stale = registry_.invoke("create.floppy", {{"planToken", token}}, context());
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "write_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale-input.ima"));
}

TEST_F(WriteOperationsTest, BuildOverwritePolicyAndCancellationPublishAtomically) {
    const auto starter = registry_.invoke("create.manifest", {{"kind", "HDS"}}, context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["size_bytes"] = 1'048'576U;
    std::ofstream{root_ / "existing.hds", std::ios::binary} << "sentinel";
    const nlohmann::json base_request = {
        {"kind", "HDS"},
        {"manifest", {{"inline", manifest}}},
        {"output", file_ref("existing.hds")},
    };

    const auto refused = registry_.invoke("create.plan", base_request, context());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "output_exists");
    EXPECT_NE(refused.error().message.find("already exists"), std::string::npos);

    auto replacement_request = base_request;
    replacement_request["overwrite"] = true;
    const auto replacement = registry_.invoke("create.plan", replacement_request, context());
    ASSERT_TRUE(replacement) << replacement.error().message;
    const auto replaced =
        registry_.invoke("create.hds", {{"planToken", replacement->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(replaced) << replaced.error().message;
    EXPECT_EQ(std::filesystem::file_size(root_ / "existing.hds"), 1'048'576U);

    auto cancelled_request = base_request;
    cancelled_request["output"] = file_ref("cancelled.hds");
    const auto cancelled_plan = registry_.invoke("create.plan", cancelled_request, context());
    ASSERT_TRUE(cancelled_plan) << cancelled_plan.error().message;
    axk::CancellationSource cancellation;
    cancellation.cancel();
    auto cancelled_context = context();
    cancelled_context.cancellation = cancellation.token();
    const auto cancelled = registry_.invoke(
        "create.hds", {{"planToken", cancelled_plan->at("planToken").get<std::string>()}}, cancelled_context);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, "operation_cancelled");
    EXPECT_FALSE(std::filesystem::exists(root_ / "cancelled.hds"));
}

TEST_F(WriteOperationsTest, AlterationRequiresExplicitAtomicSourceReplacement) {
    const auto build_starter = registry_.invoke("create.manifest", {{"kind", "HDS"}}, context());
    ASSERT_TRUE(build_starter) << build_starter.error().message;
    auto build_manifest = build_starter->at("manifest");
    build_manifest["size_bytes"] = 4U * 1024U * 1024U;
    build_manifest["partitions"][0]["volumes"] = nlohmann::json::array(
        {{{"name", "First Volume"}, {"waveforms", nlohmann::json::array()}, {"samples", nlohmann::json::array()}},
         {{"name", "Second Volume"}, {"waveforms", nlohmann::json::array()}, {"samples", nlohmann::json::array()}}});
    const auto build_plan = registry_.invoke("create.plan",
                                             {{"kind", "HDS"},
                                              {"manifest", {{"inline", build_manifest}}},
                                              {"output", {{"rootId", "workspace"}, {"relativePath", "source.hds"}}}},
                                             context());
    ASSERT_TRUE(build_plan) << build_plan.error().message;
    const auto built =
        registry_.invoke("create.hds", {{"planToken", build_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(built) << built.error().message;
    const auto starter = registry_.invoke("alter.manifest", nlohmann::json::object(), context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto alteration_manifest = starter->at("manifest");
    alteration_manifest["operations"] = nlohmann::json::array(
        {{{"id", "remove"}, {"type", "delete_volume"}, {"partition_index", 0U}, {"volume_name", "Second Volume"}}});
    const nlohmann::json inspection_request = {
        {"source", file_ref("source.hds")},
        {"manifest", {{"inline", alteration_manifest}}},
    };
    const auto inspection = registry_.invoke("alter.inspect", inspection_request, context());
    ASSERT_TRUE(inspection) << inspection.error().message;
    ASSERT_EQ(inspection->at("operations").size(), 1U);
    EXPECT_EQ(inspection->at("schemaVersion"), "1.0");
    EXPECT_EQ(inspection->at("kind"), "ALTERATION");
    EXPECT_EQ(inspection->at("summary").at("operationCount"), 1U);
    EXPECT_TRUE(inspection->at("summary").contains("freedClusters"));
    EXPECT_TRUE(inspection->at("summary").contains("allocatedClusters"));
    EXPECT_TRUE(inspection->at("warnings").empty());
    EXPECT_TRUE(inspection->at("validation").at("valid").get<bool>());
    EXPECT_FALSE(inspection->contains("planToken"));
    EXPECT_FALSE(inspection->contains("output"));
    EXPECT_EQ(inspection->dump().find(root_.string()), std::string::npos);
    auto alteration_request = inspection_request;
    alteration_request["output"] = file_ref("altered.hds");
    const auto altered = registry_.invoke("alter.hds", alteration_request, context());
    ASSERT_TRUE(altered) << altered.error().message;
    EXPECT_TRUE(altered->at("applied").get<bool>());
    EXPECT_EQ(altered->at("schemaVersion"), "1.0");
    EXPECT_EQ(altered->at("kind"), "ALTERATION");
    EXPECT_EQ(altered->at("summary").at("operationCount"), 1U);
    EXPECT_TRUE(altered->at("warnings").empty());
    EXPECT_EQ(altered->dump().find(root_.string()), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "altered.hds"));

    const auto aliased = registry_.invoke("alter.hds",
                                          {{"source", {{"rootId", "workspace"}, {"relativePath", "source.hds"}}},
                                           {"manifest", {{"inline", alteration_manifest}}},
                                           {"output", {{"rootId", "workspace"}, {"relativePath", "source.hds"}}},
                                           {"overwrite", true}},
                                          context());
    EXPECT_FALSE(aliased);

    const auto ambiguous = registry_.invoke("alter.hds",
                                            {{"source", file_ref("source.hds")},
                                             {"manifest", {{"inline", alteration_manifest}}},
                                             {"output", file_ref("other.hds")},
                                             {"replaceSource", true}},
                                            context());
    EXPECT_FALSE(ambiguous);

    const auto source_before = read_bytes(root_ / "source.hds");
    const auto replacement = registry_.invoke("alter.hds",
                                              {{"source", file_ref("source.hds")},
                                               {"manifest", {{"inline", alteration_manifest}}},
                                               {"output", file_ref("source.hds")},
                                               {"replaceSource", true}},
                                              context());
    ASSERT_TRUE(replacement) << replacement.error().message;
    EXPECT_NE(read_bytes(root_ / "source.hds"), source_before);
    const auto reopened = axk::open_image(root_ / "source.hds");
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto &root_record =
        *std::ranges::find(reopened->partitions().front().records, axk::SfsId{1}, &axk::IndexRecord::sfs_id);
    EXPECT_FALSE(std::ranges::contains(root_record.directory_entries, "Second Volume", &axk::DirectoryEntry::name));
}

TEST_F(WriteOperationsTest, AlterationInspectionCreatesNoApplyAuthority) {
    const auto starter = registry_.invoke("alter.manifest", nlohmann::json::object(), context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["operations"] = nlohmann::json::array(
        {{{"id", "remove"}, {"type", "delete_volume"}, {"partition_index", 0U}, {"volume_name", "New Volume"}}});
    const auto inspection = registry_.invoke(
        "alter.inspect", {{"source", file_ref("fixture.hds")}, {"manifest", {{"inline", manifest}}}}, context());
    ASSERT_TRUE(inspection) << inspection.error().message;
    EXPECT_FALSE(inspection->contains("planToken"));
    const auto incomplete = registry_.invoke("alter.hds", {{"source", file_ref("fixture.hds")}}, context());
    ASSERT_FALSE(incomplete);
    EXPECT_EQ(incomplete.error().code, "invalid_request");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale.hds"));
}

TEST_F(WriteOperationsTest, SessionAlterationCommitsInPlaceAndRefreshesTheExistingSession) {
    const auto opened = images_->open({"workspace", "fixture.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->revision, 1U);
    const auto objects_before = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects_before) << objects_before.error().message;

    std::vector<nlohmann::json> diagnostics;
    auto mutation_context = context();
    mutation_context.diagnostic = [&](const nlohmann::json &event) { diagnostics.push_back(event); };
    const nlohmann::json manifest = {
        {"schema_version", "1.0"},
        {"operations", nlohmann::json::array({{{"id", "rename"},
                                               {"type", "rename_volume"},
                                               {"partition_index", 0U},
                                               {"volume_name", "New Volume"},
                                               {"new_volume_name", "Renamed Volume"}}})},
    };
    const auto altered = registry_.invoke(
        "images.alter",
        {{"imageId", opened->image_id}, {"expectedRevision", opened->revision}, {"manifest", {{"inline", manifest}}}},
        mutation_context);
    ASSERT_TRUE(altered) << altered.error().message;
    EXPECT_EQ(altered->at("imageId"), opened->image_id);
    EXPECT_EQ(altered->at("revision"), 2U);
    EXPECT_TRUE(altered->at("applied").get<bool>());

    const auto inspected = images_->inspect(opened->image_id, "owner");
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->revision, 2U);
    const auto roots = images_->content(opened->image_id, "owner", 100U);
    ASSERT_TRUE(roots) << roots.error().message;
    ASSERT_EQ(roots->items.size(), 1U);
    const auto volumes = images_->content(opened->image_id, "owner", 100U, std::nullopt, roots->items.front().id);
    ASSERT_TRUE(volumes) << volumes.error().message;
    EXPECT_TRUE(std::ranges::contains(volumes->items, "Renamed Volume", &axk::app::ImageContentItem::name));
    const auto objects_after = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(objects_after) << objects_after.error().message;
    ASSERT_EQ(objects_after->items.size(), objects_before->items.size());
    for (const auto &before : objects_before->items) {
        const auto after = std::ranges::find_if(objects_after->items, [&](const auto &candidate) {
            return candidate.type == before.type && candidate.name == before.name;
        });
        ASSERT_NE(after, objects_after->items.end());
        EXPECT_EQ(after->id, before.id);
        EXPECT_EQ(after->volume_name, "Renamed Volume");
    }

    const auto planning = std::ranges::find(diagnostics, "planning",
                                            [](const auto &event) { return event.value("phase", std::string{}); });
    ASSERT_NE(planning, diagnostics.end());
    EXPECT_GT(planning->at("patchCount").get<std::size_t>(), 0U);
    EXPECT_LT(planning->at("patchBytes").get<std::uint64_t>(), planning->at("imageBytes").get<std::uint64_t>());

    const auto stale = registry_.invoke(
        "images.alter", {{"imageId", opened->image_id}, {"expectedRevision", 1U}, {"manifest", {{"inline", manifest}}}},
        context());
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "image_revision_stale");
}

TEST_F(WriteOperationsTest, SessionVolumePlacementRepairMakesDeletionSafe) {
    write_tone(root_ / "repair.wav");
    const auto source = root_ / "placement-repair.hds";
    ASSERT_TRUE(axk::write_hds_image(placement_repair_manifest(root_ / "repair.wav"), source));
    auto container = axk::open_image(source);
    ASSERT_TRUE(container) << container.error().message;
    const auto sample_directory = std::ranges::find_if(container->partitions().front().records, [](const auto &record) {
        return record.payload_kind == axk::PayloadKind::directory &&
               std::ranges::contains(record.directory_entries, "Old Sample", &axk::DirectoryEntry::name);
    });
    ASSERT_NE(sample_directory, container->partitions().front().records.end());
    ASSERT_EQ(sample_directory->directory_entries.size(), 3U);
    const auto sample_entry =
        std::ranges::find(sample_directory->directory_entries, "Old Sample", &axk::DirectoryEntry::name);
    ASSERT_NE(sample_entry, sample_directory->directory_entries.end());
    const auto sample_sfs_id = axk::SfsId{sample_entry->link_id.value};
    patch_index_be32(source, *sample_directory, 6U, 64U);
    patch_index_be32(source, *sample_directory, 0x12U, 64U);

    container = axk::open_image(source);
    ASSERT_TRUE(container) << container.error().message;
    const auto before = axk::build_object_catalog(*container);
    ASSERT_TRUE(before) << before.error().message;
    const auto missing = std::ranges::find_if(before->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == sample_sfs_id;
    });
    ASSERT_NE(missing, before->objects.end());
    ASSERT_EQ(missing->placement_resolution, axk::PlacementResolution::missing);
    const auto payload_before = missing->raw_payload;

    const auto opened = images_->open({"workspace", "placement-repair.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const nlohmann::json request = {{"imageId", opened->image_id},
                                    {"expectedRevision", opened->revision},
                                    {"partitionIndex", 0U},
                                    {"volumeName", "Samples"}};
    const auto inspected = registry_.invoke("images.volume_deletion.inspect", request, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->at("canDelete").get<bool>());
    EXPECT_EQ(inspected->at("crossingRelationshipCount"), 1U);
    ASSERT_EQ(inspected->at("blockers").size(), 1U);

    auto absent_volume_request = request;
    absent_volume_request["volumeName"] = "Absent";
    const auto absent_volume = registry_.invoke("images.volume_deletion.inspect", absent_volume_request, context());
    ASSERT_FALSE(absent_volume);
    EXPECT_EQ(absent_volume.error().code, "volume_scope_invalid");

    const nlohmann::json placement_request = {
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"scope", {{"kind", "VOLUME"}, {"partitionIndex", 0U}, {"volumeName", "Samples"}}},
    };
    const auto placement = registry_.invoke("images.placement.inspect", placement_request, context());
    ASSERT_TRUE(placement) << placement.error().message;
    EXPECT_TRUE(placement->at("canRepair").get<bool>());
    EXPECT_EQ(placement->at("repairObjectCount"), 1U);
    EXPECT_EQ(placement->at("blockedObjectCount"), 0U);
    ASSERT_EQ(placement->at("destinations").size(), 1U);
    EXPECT_EQ(placement->at("destinations").front().at("volumeName"), "Samples");
    EXPECT_FALSE(placement->at("destinations").front().at("createsVolume").get<bool>());
    EXPECT_EQ(placement->at("destinations").front().at("objectTypeCounts").at("SBNK"), 1U);

    const auto repaired = registry_.invoke("images.placement.repair", placement_request, context());
    ASSERT_TRUE(repaired) << repaired.error().message;
    EXPECT_EQ(repaired->at("revision"), 2U);
    ASSERT_EQ(repaired->at("operations").size(), 1U);
    EXPECT_EQ(repaired->at("operations").front().at("type"), "REPAIR_OBJECT_PLACEMENTS");
    EXPECT_EQ(repaired->at("operations").front().at("placedSfsIds"), nlohmann::json::array({sample_sfs_id.value}));

    auto follow_up_request = request;
    follow_up_request["expectedRevision"] = 2U;
    const auto follow_up = registry_.invoke("images.volume_deletion.inspect", follow_up_request, context());
    ASSERT_TRUE(follow_up) << follow_up.error().message;
    EXPECT_TRUE(follow_up->at("canDelete").get<bool>());
    EXPECT_EQ(follow_up->at("crossingRelationshipCount"), 0U);
    EXPECT_TRUE(follow_up->at("blockers").empty());

    const auto after_container = axk::open_image(source);
    ASSERT_TRUE(after_container) << after_container.error().message;
    const auto after = axk::build_object_catalog(*after_container);
    ASSERT_TRUE(after) << after.error().message;
    const auto placed = std::ranges::find_if(after->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == sample_sfs_id;
    });
    ASSERT_NE(placed, after->objects.end());
    ASSERT_TRUE(placed->placement);
    EXPECT_EQ(placed->placement_resolution, axk::PlacementResolution::exact);
    EXPECT_EQ(placed->placement->volume_name, "Samples");
    EXPECT_EQ(placed->raw_payload, payload_before);
}

TEST_F(WriteOperationsTest, SessionPartitionPlacementRepairRecoversOwnerlessWaveDataIntoNewVolume) {
    write_tone(root_ / "ownerless.wav");
    const auto source = root_ / "partition-placement-repair.hds";
    auto source_manifest = placement_repair_manifest(root_ / "ownerless.wav");
    source_manifest.partitions.front().volumes.front().samples.clear();
    ASSERT_TRUE(axk::write_hds_image(source_manifest, source));
    auto container = axk::open_image(source);
    ASSERT_TRUE(container) << container.error().message;
    const auto waveform_directory =
        std::ranges::find_if(container->partitions().front().records, [](const auto &record) {
            return record.payload_kind == axk::PayloadKind::directory &&
                   std::ranges::contains(record.directory_entries, "Wave", &axk::DirectoryEntry::name);
        });
    ASSERT_NE(waveform_directory, container->partitions().front().records.end());
    const auto waveform_entry =
        std::ranges::find(waveform_directory->directory_entries, "Wave", &axk::DirectoryEntry::name);
    ASSERT_NE(waveform_entry, waveform_directory->directory_entries.end());
    const auto waveform_sfs_id = axk::SfsId{waveform_entry->link_id.value};
    patch_index_be32(source, *waveform_directory, 6U, 64U);
    patch_index_be32(source, *waveform_directory, 0x12U, 64U);

    const auto opened = images_->open({"workspace", "partition-placement-repair.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const nlohmann::json request = {
        {"imageId", opened->image_id},
        {"expectedRevision", opened->revision},
        {"scope", {{"kind", "PARTITION"}, {"partitionIndex", 0U}}},
    };
    const auto inspected = registry_.invoke("images.placement.inspect", request, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->at("canRepair").get<bool>());
    EXPECT_EQ(inspected->at("repairObjectCount"), 1U);
    EXPECT_EQ(inspected->at("recoveryVolumeName"), "Recovered");
    ASSERT_EQ(inspected->at("destinations").size(), 1U);
    EXPECT_TRUE(inspected->at("destinations").front().at("createsVolume").get<bool>());
    EXPECT_EQ(inspected->at("destinations").front().at("objectTypeCounts").at("SMPL"), 1U);

    const auto repaired = registry_.invoke("images.placement.repair", request, context());
    ASSERT_TRUE(repaired) << repaired.error().message;
    EXPECT_EQ(repaired->at("revision"), 2U);
    ASSERT_EQ(repaired->at("operations").size(), 2U);
    EXPECT_EQ(repaired->at("operations").front().at("type"), "INSERT_VOLUME");
    EXPECT_EQ(repaired->at("operations").back().at("type"), "REPAIR_OBJECT_PLACEMENTS");
    EXPECT_EQ(repaired->at("operations").back().at("placedSfsIds"), nlohmann::json::array({waveform_sfs_id.value}));

    container = axk::open_image(source);
    ASSERT_TRUE(container) << container.error().message;
    const auto after = axk::build_object_catalog(*container);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_TRUE(after->issues.empty());
    const auto placed = std::ranges::find_if(after->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == waveform_sfs_id;
    });
    ASSERT_NE(placed, after->objects.end());
    ASSERT_TRUE(placed->placement);
    EXPECT_EQ(placed->placement_resolution, axk::PlacementResolution::exact);
    EXPECT_EQ(placed->placement->volume_name, "Recovered");
}

TEST_F(WriteOperationsTest, SessionObjectDeletionInspectsAndCommitsTheReviewedClosure) {
    const auto opened = images_->open({"workspace", "fixture.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = images_->content(opened->image_id, "owner", 100U);
    ASSERT_TRUE(roots) << roots.error().message;
    ASSERT_EQ(roots->items.size(), 1U);
    const auto volumes = images_->content(opened->image_id, "owner", 100U, std::nullopt, roots->items.front().id);
    ASSERT_TRUE(volumes) << volumes.error().message;
    const auto volume = std::ranges::find(volumes->items, "volume", &axk::app::ImageContentItem::kind);
    ASSERT_NE(volume, volumes->items.end());
    const auto orphans = registry_.invoke(
        "images.deletion.orphans.inspect",
        {{"imageId", opened->image_id}, {"expectedRevision", opened->revision}, {"contentScopeId", volume->id}},
        context());
    ASSERT_TRUE(orphans) << orphans.error().message;
    EXPECT_EQ(orphans->at("contentScopeId"), volume->id);
    EXPECT_EQ(orphans->at("totalCandidateCount"), orphans->at("candidates").size());

    const auto samples = images_->objects(opened->image_id, "owner", 100U, std::nullopt, "SBNK");
    ASSERT_TRUE(samples) << samples.error().message;
    const auto sample = std::ranges::find(samples->items, "sine wave", &axk::app::ImageObjectItem::name);
    ASSERT_NE(sample, samples->items.end());
    const auto object_count = opened->object_count;

    const auto inspection = registry_.invoke("images.deletion.inspect",
                                             {{"imageId", opened->image_id},
                                              {"expectedRevision", opened->revision},
                                              {"targetObjectIds", nlohmann::json::array({sample->id})},
                                              {"cleanupObjectIds", nlohmann::json::array()}},
                                             context());
    ASSERT_TRUE(inspection) << inspection.error().message;
    EXPECT_TRUE(inspection->at("canApply").get<bool>());
    const auto optional_wave = std::ranges::find(inspection->at("impacts"), "SMPL", [](const auto &impact) {
        return impact.at("objectType").template get<std::string>();
    });
    ASSERT_NE(optional_wave, inspection->at("impacts").end());
    EXPECT_EQ(optional_wave->at("status"), "OPTIONAL");
    EXPECT_TRUE(optional_wave->at("partitionIndex").is_number_unsigned());
    ASSERT_FALSE(inspection->at("references").empty());
    EXPECT_TRUE(inspection->at("references").front().at("targetObjectId").is_string());
    EXPECT_TRUE(inspection->at("references").front().at("targetObjectType").is_string());
    EXPECT_TRUE(inspection->at("references").front().at("targetObjectName").is_string());

    const std::vector included{optional_wave->at("objectId").get<std::string>()};
    auto moved_registry = std::move(registry_);
    const auto deleted = moved_registry.invoke("images.delete",
                                               {{"imageId", opened->image_id},
                                                {"expectedRevision", opened->revision},
                                                {"targetObjectIds", nlohmann::json::array({sample->id})},
                                                {"cleanupObjectIds", included}},
                                               context());
    ASSERT_TRUE(deleted) << deleted.error().message;
    EXPECT_EQ(deleted->at("kind"), "DELETION");
    EXPECT_EQ(deleted->at("revision"), 2U);
    EXPECT_EQ(deleted->at("deletedObjectIds").size(), 2U);
    EXPECT_TRUE(deleted->at("blockedObjectIds").empty());
    EXPECT_GT(deleted->at("freedClusters").get<std::uint64_t>(), 0U);
    EXPECT_EQ(deleted->at("objectCount"), object_count - 2U);

    const auto remaining = images_->objects(opened->image_id, "owner", 100U);
    ASSERT_TRUE(remaining) << remaining.error().message;
    EXPECT_FALSE(std::ranges::contains(remaining->items, sample->id, &axk::app::ImageObjectItem::id));
    EXPECT_FALSE(std::ranges::contains(remaining->items, included.front(), &axk::app::ImageObjectItem::id));
}

TEST_F(WriteOperationsTest, SessionProgramGenerationInspectsAndCommitsSamplerControlledPrograms) {
    write_tone(root_ / "program-generation.wav");
    const auto written = axk::write_hds_image(all_action_source_manifest(root_ / "program-generation.wav"),
                                              root_ / "program-generation.hds");
    ASSERT_TRUE(written) << written.error().message;
    const auto opened = images_->open({"workspace", "program-generation.hds"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = images_->content(opened->image_id, "owner", 100U);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto volumes = images_->content(opened->image_id, "owner", 100U, std::nullopt, roots->items.front().id);
    ASSERT_TRUE(volumes) << volumes.error().message;
    const auto volume = std::ranges::find(volumes->items, "volume", &axk::app::ImageContentItem::kind);
    ASSERT_NE(volume, volumes->items.end());

    const auto inspection = registry_.invoke(
        "images.programs.generate.inspect",
        {{"imageId", opened->image_id}, {"expectedRevision", opened->revision}, {"contentScopeId", volume->id}},
        context());
    ASSERT_TRUE(inspection) << inspection.error().message;
    ASSERT_FALSE(inspection->at("candidates").empty());
    const auto &candidate = inspection->at("candidates").front();
    ASSERT_TRUE(candidate.at("defaultSelected").get<bool>());

    const auto generated =
        registry_.invoke("images.programs.generate",
                         {{"imageId", opened->image_id},
                          {"expectedRevision", opened->revision},
                          {"contentScopeId", volume->id},
                          {"programs", nlohmann::json::array({{{"targetObjectId", candidate.at("targetObjectId")},
                                                               {"programNumber", candidate.at("programNumber")},
                                                               {"programName", candidate.at("defaultProgramName")}}})}},
                         context());
    ASSERT_TRUE(generated) << generated.error().message;
    EXPECT_EQ(generated->at("kind"), "PROGRAM_GENERATION");
    EXPECT_EQ(generated->at("revision"), 2U);
    ASSERT_EQ(generated->at("createdPrograms").size(), 1U);
    EXPECT_EQ(generated->at("createdPrograms").front().at("targetObjectId"), candidate.at("targetObjectId"));

    const auto programs = images_->objects(opened->image_id, "owner", 128U, std::nullopt, "PROG");
    ASSERT_TRUE(programs) << programs.error().message;
    const auto generated_slot = std::format("{:03}", candidate.at("programNumber").get<unsigned int>());
    EXPECT_NE(std::ranges::find(programs->items, generated_slot, &axk::app::ImageObjectItem::entry_name),
              programs->items.end());
}

TEST_F(WriteOperationsTest, AlterationRechecksInputsBeforePublishing) {
    const auto starter = registry_.invoke("alter.manifest", nlohmann::json::object(), context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["operations"] = nlohmann::json::array(
        {{{"id", "remove"}, {"type", "delete_volume"}, {"partition_index", 0U}, {"volume_name", "New Volume"}}});
    const nlohmann::json request = {{"source", file_ref("fixture.hds")},
                                    {"manifest", {{"inline", manifest}}},
                                    {"output", file_ref("changed-during-apply.hds")}};

    MutateSourceOnCompletedWrite progress{root_ / "fixture.hds"};
    auto apply_context = context();
    apply_context.progress = &progress;
    const auto applied = registry_.invoke("alter.hds", request, apply_context);

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, "input_changed");
    EXPECT_FALSE(std::filesystem::exists(root_ / "changed-during-apply.hds"));
    EXPECT_FALSE(has_publication_temporary(root_));
}

TEST_F(WriteOperationsTest, AlterationCancellationAtEveryMutationPhasePublishesNothing) {
    const auto starter = registry_.invoke("alter.manifest", nlohmann::json::object(), context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["operations"] = nlohmann::json::array(
        {{{"id", "remove"}, {"type", "delete_volume"}, {"partition_index", 0U}, {"volume_name", "New Volume"}}});
    for (const auto phase : {axk::ProgressPhase::allocating, axk::ProgressPhase::writing,
                             axk::ProgressPhase::validating, axk::ProgressPhase::publishing}) {
        const auto output = std::format("cancelled-{}.hds", static_cast<unsigned int>(phase));
        const nlohmann::json request = {
            {"source", file_ref("fixture.hds")}, {"manifest", {{"inline", manifest}}}, {"output", file_ref(output)}};

        axk::CancellationSource cancellation;
        CancelOnPhase progress{cancellation, phase};
        auto apply_context = context();
        apply_context.cancellation = cancellation.token();
        apply_context.progress = &progress;
        const auto applied = registry_.invoke("alter.hds", request, apply_context);

        ASSERT_FALSE(applied) << static_cast<unsigned int>(phase);
        EXPECT_EQ(applied.error().code, "operation_cancelled") << static_cast<unsigned int>(phase);
        EXPECT_FALSE(std::filesystem::exists(root_ / output)) << static_cast<unsigned int>(phase);
        EXPECT_FALSE(has_publication_temporary(root_)) << static_cast<unsigned int>(phase);
    }
}

TEST_F(WriteOperationsTest, AlterationInspectsAndAppliesEveryMaintainedAction) {
    write_tone(root_ / "insert.wav");
    const auto written =
        axk::write_hds_image(all_action_source_manifest(root_ / "insert.wav"), root_ / "all-actions.hds");
    ASSERT_TRUE(written) << written.error().message;
    const auto source_before = read_bytes(root_ / "all-actions.hds");
    const auto manifest = all_action_alteration_manifest();
    const nlohmann::json inspection_request = {
        {"source", file_ref("all-actions.hds")},
        {"manifest", {{"inline", manifest}}},
        {"inputBindings", {{{"manifestPath", "insert.wav"}, {"input", {{"fileRef", file_ref("insert.wav")}}}}}},
    };
    const auto inspected = registry_.invoke("alter.inspect", inspection_request, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    ASSERT_EQ(inspected->at("operations").size(), 14U);
    EXPECT_EQ(inspected->at("summary").at("operationCount"), 14U);
    EXPECT_EQ(inspected->at("operations")[4].at("audioImport").at("sourcePath"), "insert.wav");

    auto alteration_request = inspection_request;
    alteration_request["output"] = file_ref("all-actions-altered.hds");
    const auto applied = registry_.invoke("alter.hds", alteration_request, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->at("operations"), inspected->at("operations"));
    EXPECT_EQ(applied->at("summary"), inspected->at("summary"));
    EXPECT_TRUE(applied->at("validation").at("valid").get<bool>());
    EXPECT_EQ(read_bytes(root_ / "all-actions.hds"), source_before);
    EXPECT_EQ(applied->dump().find(root_.string()), std::string::npos);

    const auto reopened = axk::open_image(root_ / "all-actions-altered.hds");
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto has_object = [&](axk::ObjectType type, std::string_view name) {
        return std::ranges::any_of(catalog->objects, [&](const auto &object) {
            return object.object.header.type == type && object.object.header.name == name;
        });
    };
    EXPECT_TRUE(has_object(axk::ObjectType::smpl, "Insert Wave"));
    EXPECT_TRUE(has_object(axk::ObjectType::smpl, "New Wave"));
    EXPECT_FALSE(has_object(axk::ObjectType::smpl, "Delete Wave"));
    EXPECT_TRUE(has_object(axk::ObjectType::sbnk, "Insert Sample"));
    EXPECT_TRUE(has_object(axk::ObjectType::sbnk, "New Sample"));
    EXPECT_FALSE(has_object(axk::ObjectType::sbnk, "Delete Sample"));
    EXPECT_TRUE(has_object(axk::ObjectType::sbac, "Insert Bank"));
    EXPECT_TRUE(has_object(axk::ObjectType::sbac, "New Bank"));
    EXPECT_FALSE(has_object(axk::ObjectType::sbac, "Delete Bank"));
    EXPECT_TRUE(has_object(axk::ObjectType::prog, "128"));
    const auto renamed_program = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::prog && object.object.header.name == "127";
    });
    ASSERT_NE(renamed_program, catalog->objects.end());
    const auto *program = std::get_if<axk::CurrentProg>(&renamed_program->object.payload);
    ASSERT_NE(program, nullptr);
    EXPECT_EQ(program->program_name, "Renamed");
    const auto inserted = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::smpl && object.object.header.name == "Insert Wave";
    });
    ASSERT_NE(inserted, catalog->objects.end());
    const auto decoded = axk::decode_waveform(*reopened, *inserted);
    ASSERT_TRUE(decoded) << decoded.error().message;
    std::vector expected_pcm{std::byte{},     std::byte{},     std::byte{0xe8}, std::byte{0x03},
                             std::byte{0x18}, std::byte{0xfc}, std::byte{},     std::byte{},
                             std::byte{0xe8}, std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
    EXPECT_EQ(decoded->pcm, expected_pcm);
}

TEST_F(WriteOperationsTest, AlterationRejectsMissingInputBindings) {
    write_tone(root_ / "insert.wav");
    const auto written =
        axk::write_hds_image(all_action_source_manifest(root_ / "insert.wav"), root_ / "bound-inputs.hds");
    ASSERT_TRUE(written) << written.error().message;
    const auto manifest = all_action_alteration_manifest();
    std::ofstream{root_ / "alteration.json", std::ios::binary} << manifest.dump();
    const nlohmann::json request = {
        {"source", file_ref("bound-inputs.hds")},
        {"manifest", {{"fileRef", file_ref("alteration.json")}}},
        {"inputBindings", {{{"manifestPath", "insert.wav"}, {"input", {{"fileRef", file_ref("insert.wav")}}}}}},
        {"output", file_ref("stale-manifest.hds")},
    };

    auto missing_binding = request;
    missing_binding.erase("inputBindings");
    const auto missing = registry_.invoke("alter.hds", missing_binding, context());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "missing_input_binding");

    const auto inspected = registry_.invoke("alter.inspect", request, context());
    ASSERT_TRUE(inspected) << inspected.error().message;
    const auto applied = registry_.invoke("alter.hds", request, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(std::filesystem::exists(root_ / "stale-manifest.hds"));
    EXPECT_FALSE(has_publication_temporary(root_));
}

TEST_F(WriteOperationsTest, AlterationConsumesUploadedManifestAndAudioDirectly) {
    write_tone(root_ / "insert.wav");
    const auto written =
        axk::write_hds_image(all_action_source_manifest(root_ / "insert.wav"), root_ / "upload-source.hds");
    ASSERT_TRUE(written) << written.error().message;
    const auto audio_bytes = read_bytes(root_ / "insert.wav");
    const auto manifest_text = all_action_alteration_manifest().dump();

    auto audio = uploads_->create({.owner_id = "owner",
                                   .filename = "insert.wav",
                                   .kind = axk::app::UploadKind::audio,
                                   .media_type = "audio/wav",
                                   .declared_size = audio_bytes.size(),
                                   .sha256 = std::nullopt});
    ASSERT_TRUE(audio) << audio.error().message;
    ASSERT_TRUE(uploads_->append(audio->reference, "owner", 0U, audio_bytes));
    ASSERT_TRUE(uploads_->complete(audio->reference, "owner"));
    auto manifest = uploads_->create({.owner_id = "owner",
                                      .filename = "alteration.json",
                                      .kind = axk::app::UploadKind::manifest,
                                      .media_type = "application/json",
                                      .declared_size = manifest_text.size(),
                                      .sha256 = std::nullopt});
    ASSERT_TRUE(manifest) << manifest.error().message;
    ASSERT_TRUE(uploads_->append(manifest->reference, "owner", 0U, std::as_bytes(std::span{manifest_text})));
    ASSERT_TRUE(uploads_->complete(manifest->reference, "owner"));

    const nlohmann::json request = {
        {"source", file_ref("upload-source.hds")},
        {"manifest", {{"uploadRef", {{"uploadId", manifest->reference.upload_id}}}}},
        {"inputBindings",
         {{{"manifestPath", "insert.wav"}, {"input", {{"uploadRef", {{"uploadId", audio->reference.upload_id}}}}}}}},
        {"output", file_ref("upload-altered.hds")},
    };
    const auto applied = registry_.invoke("alter.hds", request, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "upload-altered.hds"));
    EXPECT_TRUE(uploads_->inspect(audio->reference, "owner"));
    EXPECT_TRUE(uploads_->inspect(manifest->reference, "owner"));

    now_ += std::chrono::seconds{6};
    uploads_->cleanup();
    const auto expired_audio = uploads_->inspect(audio->reference, "owner");
    const auto expired_manifest = uploads_->inspect(manifest->reference, "owner");
    ASSERT_FALSE(expired_audio);
    ASSERT_FALSE(expired_manifest);
    EXPECT_EQ(expired_audio.error().code, "upload_not_found");
    EXPECT_EQ(expired_manifest.error().code, "upload_not_found");
}

TEST_F(WriteOperationsTest, AlterationInspectionsDoNotReserveDestinations) {
    const auto starter = registry_.invoke("alter.manifest", nlohmann::json::object(), context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["operations"] = nlohmann::json::array(
        {{{"id", "remove"}, {"type", "delete_volume"}, {"partition_index", 0U}, {"volume_name", "New Volume"}}});
    const nlohmann::json request = {
        {"source", file_ref("fixture.hds")},
        {"manifest", {{"inline", manifest}}},
        {"output", file_ref("reserved-alteration.hds")},
    };
    auto inspection_request = request;
    inspection_request.erase("output");
    const auto first = registry_.invoke("alter.inspect", inspection_request, context());
    ASSERT_TRUE(first) << first.error().message;
    const auto second = registry_.invoke("alter.inspect", inspection_request, context());
    ASSERT_TRUE(second) << second.error().message;
    const auto applied = registry_.invoke("alter.hds", request, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(std::filesystem::exists(root_ / "reserved-alteration.hds"));
}

TEST_F(WriteOperationsTest, PlansReserveDestinationsAndReleaseThemWhenTheyBecomeStale) {
    const auto starter = registry_.invoke("create.manifest", {{"kind", "HDS"}}, context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["size_bytes"] = 1'048'576U;
    const auto request = nlohmann::json{
        {"kind", "HDS"},
        {"manifest", {{"inline", manifest}}},
        {"output", {{"rootId", "workspace"}, {"relativePath", "reserved.hds"}}},
    };
    const auto first = registry_.invoke("create.plan", request, context());
    ASSERT_TRUE(first) << first.error().message;
    const auto second = registry_.invoke("create.plan", request, context());
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, "destination_reserved");

    std::ofstream{root_ / "reserved.hds", std::ios::binary} << "appeared after planning";
    const auto stale =
        registry_.invoke("create.hds", {{"planToken", first->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "write_plan_stale");

    std::filesystem::remove(root_ / "reserved.hds");
    const auto replacement = registry_.invoke("create.plan", request, context());
    ASSERT_TRUE(replacement) << replacement.error().message;
}

} // namespace
