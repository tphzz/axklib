#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/write_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
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
    for (const auto *name : {"Delete Bank", "Old Bank", "Bank A", "Bank B", "Del Group Bank", "Delete Direct",
                             "Old Group Bank", "Old Direct"}) {
        axk::SampleBankSpec bank;
        bank.name = name;
        bank.waveform_id = "wave";
        bank.root_key = 60U;
        bank.key_high = 127U;
        volume.sample_banks.push_back(std::move(bank));
    }
    volume.sample_bank_groups = {
        {"Delete Group", {"Del Group Bank"}},
        {"Old Group", {"Old Group Bank"}},
    };
    volume.programs = {
        {128U, {{"SBAC", "Delete Group", 1U}, {"SBNK", "Delete Direct", 2U}}},
        {127U, {{"SBAC", "Old Group", 1U}, {"SBNK", "Old Direct", 2U}}},
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
                {"sample_banks", nlohmann::json::array()}}}},
             {{"id", "delete-bank"},
              {"type", "delete_sbnk"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"sample_bank_name", "Delete Bank"}},
             {{"id", "insert-bank"},
              {"type", "insert_sbnk"},
              {"partition_index", {{"operation_ref", "delete-bank"}}},
              {"volume_name", "Volume"},
              {"sample_bank",
               {{"name", "Insert Bank"},
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
             {{"id", "rename-bank"},
              {"type", "rename_sbnk"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"sample_bank_name", "Old Bank"},
              {"new_sample_bank_name", "New Bank"}},
             {{"id", "delete-program"},
              {"type", "delete_program"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"program_number", 128U}},
             {{"id", "delete-group"},
              {"type", "delete_sbac"},
              {"partition_index", {{"operation_ref", "delete-program"}}},
              {"volume_name", "Volume"},
              {"sample_bank_group_name", "Delete Group"}},
             {{"id", "insert-group"},
              {"type", "insert_sbac"},
              {"partition_index", {{"operation_ref", "delete-group"}}},
              {"volume_name", "Volume"},
              {"sample_bank_group", {{"name", "Insert Group"}, {"member_sample_banks", {"Bank A", "Bank B"}}}}},
             {{"id", "rename-group"},
              {"type", "rename_sbac"},
              {"partition_index", 0U},
              {"volume_name", "Volume"},
              {"sample_bank_group_name", "Old Group"},
              {"new_sample_bank_group_name", "New Group"}},
             {{"id", "insert-program"},
              {"type", "insert_program"},
              {"partition_index", {{"operation_ref", "rename-group"}}},
              {"volume_name", "Volume"},
              {"program",
               {{"number", 128U},
                {"assignments",
                 {{{"sample_bank_group", "Insert Group"}, {"receive_channel", 1U}},
                  {{"sample_bank", "Delete Direct"}, {"receive_channel", 2U}}}}}}},
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
        std::filesystem::create_directories(root_ / "uploads");
        std::filesystem::copy_file(fixture_path(), root_ / "fixture.hds");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox);
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        uploads_ = std::make_unique<axk::app::UploadStore>(root_ / "uploads", 16U * 1024U * 1024U, 8U * 1024U * 1024U,
                                                           8U, 2U * 1024U * 1024U, std::chrono::seconds{5},
                                                           [this] { return now_; });
        registry_ = axk::app::make_operation_registry();
        ASSERT_TRUE(axk::app::bind_write_operations(registry_, *sandbox_, *uploads_));
    }

    void TearDown() override {
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
    axk::app::OperationRegistry registry_;
};

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

TEST_F(WriteOperationsTest, AlterationAppliesWithoutAllowingInPlaceMutation) {
    const auto build_starter = registry_.invoke("create.manifest", {{"kind", "HDS"}}, context());
    ASSERT_TRUE(build_starter) << build_starter.error().message;
    auto build_manifest = build_starter->at("manifest");
    build_manifest["size_bytes"] = 4U * 1024U * 1024U;
    auto second_volume = build_manifest["partitions"][0]["volumes"][0];
    second_volume["name"] = "Second Volume";
    build_manifest["partitions"][0]["volumes"].push_back(std::move(second_volume));
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
    const auto alteration_plan =
        registry_.invoke("alter.plan",
                         {{"source", {{"rootId", "workspace"}, {"relativePath", "source.hds"}}},
                          {"manifest", {{"inline", alteration_manifest}}},
                          {"output", {{"rootId", "workspace"}, {"relativePath", "altered.hds"}}}},
                         context());
    ASSERT_TRUE(alteration_plan) << alteration_plan.error().message;
    ASSERT_EQ(alteration_plan->at("operations").size(), 1U);
    EXPECT_EQ(alteration_plan->at("schemaVersion"), "1.0");
    EXPECT_EQ(alteration_plan->at("kind"), "ALTERATION");
    EXPECT_EQ(alteration_plan->at("summary").at("operationCount"), 1U);
    EXPECT_TRUE(alteration_plan->at("summary").contains("freedClusters"));
    EXPECT_TRUE(alteration_plan->at("summary").contains("allocatedClusters"));
    EXPECT_TRUE(alteration_plan->at("warnings").empty());
    EXPECT_TRUE(alteration_plan->at("validation").at("valid").get<bool>());
    EXPECT_EQ(alteration_plan->dump().find(root_.string()), std::string::npos);
    const auto altered =
        registry_.invoke("alter.hds", {{"planToken", alteration_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(altered) << altered.error().message;
    EXPECT_TRUE(altered->at("applied").get<bool>());
    EXPECT_EQ(altered->at("schemaVersion"), "1.0");
    EXPECT_EQ(altered->at("kind"), "ALTERATION");
    EXPECT_EQ(altered->at("summary").at("operationCount"), 1U);
    EXPECT_TRUE(altered->at("warnings").empty());
    EXPECT_EQ(altered->dump().find(root_.string()), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "altered.hds"));

    const auto aliased = registry_.invoke("alter.plan",
                                          {{"source", {{"rootId", "workspace"}, {"relativePath", "source.hds"}}},
                                           {"manifest", {{"inline", alteration_manifest}}},
                                           {"output", {{"rootId", "workspace"}, {"relativePath", "source.hds"}}},
                                           {"overwrite", true}},
                                          context());
    EXPECT_FALSE(aliased);
}

TEST_F(WriteOperationsTest, PlanTokensAreOwnerBoundAndRejectStaleAlterationSources) {
    const auto starter = registry_.invoke("alter.manifest", nlohmann::json::object(), context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["operations"] = nlohmann::json::array(
        {{{"id", "remove"}, {"type", "delete_volume"}, {"partition_index", 0U}, {"volume_name", "New Volume"}}});
    const auto planned = registry_.invoke("alter.plan",
                                          {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}},
                                           {"manifest", {{"inline", manifest}}},
                                           {"output", {{"rootId", "workspace"}, {"relativePath", "stale.hds"}}}},
                                          context());
    ASSERT_TRUE(planned) << planned.error().message;
    const auto token = planned->at("planToken").get<std::string>();

    auto other = context();
    other.owner_id = "other";
    const auto denied = registry_.invoke("alter.hds", {{"planToken", token}}, other);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "write_plan_not_found");

    std::ofstream{root_ / "fixture.hds", std::ios::binary | std::ios::app} << "changed";
    const auto stale = registry_.invoke("alter.hds", {{"planToken", token}}, context());
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "write_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale.hds"));
}

TEST_F(WriteOperationsTest, AlterationRechecksInputsBeforePublishing) {
    const auto starter = registry_.invoke("alter.manifest", nlohmann::json::object(), context());
    ASSERT_TRUE(starter) << starter.error().message;
    auto manifest = starter->at("manifest");
    manifest["operations"] = nlohmann::json::array(
        {{{"id", "remove"}, {"type", "delete_volume"}, {"partition_index", 0U}, {"volume_name", "New Volume"}}});
    const auto planned = registry_.invoke("alter.plan",
                                          {{"source", file_ref("fixture.hds")},
                                           {"manifest", {{"inline", manifest}}},
                                           {"output", file_ref("changed-during-apply.hds")}},
                                          context());
    ASSERT_TRUE(planned) << planned.error().message;

    MutateSourceOnCompletedWrite progress{root_ / "fixture.hds"};
    auto apply_context = context();
    apply_context.progress = &progress;
    const auto applied =
        registry_.invoke("alter.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, apply_context);

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, "write_plan_stale");
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
        const auto planned = registry_.invoke(
            "alter.plan",
            {{"source", file_ref("fixture.hds")}, {"manifest", {{"inline", manifest}}}, {"output", file_ref(output)}},
            context());
        ASSERT_TRUE(planned) << planned.error().message;

        axk::CancellationSource cancellation;
        CancelOnPhase progress{cancellation, phase};
        auto apply_context = context();
        apply_context.cancellation = cancellation.token();
        apply_context.progress = &progress;
        const auto applied =
            registry_.invoke("alter.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, apply_context);

        ASSERT_FALSE(applied) << static_cast<unsigned int>(phase);
        EXPECT_EQ(applied.error().code, "operation_cancelled") << static_cast<unsigned int>(phase);
        EXPECT_FALSE(std::filesystem::exists(root_ / output)) << static_cast<unsigned int>(phase);
        EXPECT_FALSE(has_publication_temporary(root_)) << static_cast<unsigned int>(phase);
    }
}

TEST_F(WriteOperationsTest, AlterationPlansAndAppliesEveryMaintainedAction) {
    write_tone(root_ / "insert.wav");
    const auto written =
        axk::write_hds_image(all_action_source_manifest(root_ / "insert.wav"), root_ / "all-actions.hds");
    ASSERT_TRUE(written) << written.error().message;
    const auto source_before = read_bytes(root_ / "all-actions.hds");
    const auto manifest = all_action_alteration_manifest();
    const auto planned = registry_.invoke(
        "alter.plan",
        {{"source", file_ref("all-actions.hds")},
         {"manifest", {{"inline", manifest}}},
         {"inputBindings", {{{"manifestPath", "insert.wav"}, {"input", {{"fileRef", file_ref("insert.wav")}}}}}},
         {"output", file_ref("all-actions-altered.hds")}},
        context());
    ASSERT_TRUE(planned) << planned.error().message;
    ASSERT_EQ(planned->at("operations").size(), 13U);
    EXPECT_EQ(planned->at("summary").at("operationCount"), 13U);
    EXPECT_EQ(planned->at("operations")[4].at("audioImport").at("sourcePath"), "insert.wav");

    const auto applied =
        registry_.invoke("alter.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->at("operations"), planned->at("operations"));
    EXPECT_EQ(applied->at("summary"), planned->at("summary"));
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
    EXPECT_TRUE(has_object(axk::ObjectType::sbnk, "Insert Bank"));
    EXPECT_TRUE(has_object(axk::ObjectType::sbnk, "New Bank"));
    EXPECT_FALSE(has_object(axk::ObjectType::sbnk, "Delete Bank"));
    EXPECT_TRUE(has_object(axk::ObjectType::sbac, "Insert Group"));
    EXPECT_TRUE(has_object(axk::ObjectType::sbac, "New Group"));
    EXPECT_FALSE(has_object(axk::ObjectType::sbac, "Delete Group"));
    EXPECT_TRUE(has_object(axk::ObjectType::prog, "128"));
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

TEST_F(WriteOperationsTest, AlterationRejectsMissingChangedManifestAndChangedAudioInputs) {
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
    const auto missing = registry_.invoke("alter.plan", missing_binding, context());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "missing_input_binding");

    const auto manifest_plan = registry_.invoke("alter.plan", request, context());
    ASSERT_TRUE(manifest_plan) << manifest_plan.error().message;
    std::ofstream{root_ / "alteration.json", std::ios::binary | std::ios::app} << '\n';
    const auto stale_manifest =
        registry_.invoke("alter.hds", {{"planToken", manifest_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(stale_manifest);
    EXPECT_EQ(stale_manifest.error().code, "write_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale-manifest.hds"));

    std::ofstream{root_ / "alteration.json", std::ios::binary | std::ios::trunc} << manifest.dump();
    auto audio_request = request;
    audio_request["output"] = file_ref("stale-audio.hds");
    const auto audio_plan = registry_.invoke("alter.plan", audio_request, context());
    ASSERT_TRUE(audio_plan) << audio_plan.error().message;
    std::ofstream{root_ / "insert.wav", std::ios::binary | std::ios::app} << "changed";
    const auto stale_audio =
        registry_.invoke("alter.hds", {{"planToken", audio_plan->at("planToken").get<std::string>()}}, context());
    ASSERT_FALSE(stale_audio);
    EXPECT_EQ(stale_audio.error().code, "write_plan_stale");
    EXPECT_FALSE(std::filesystem::exists(root_ / "stale-audio.hds"));
    EXPECT_FALSE(has_publication_temporary(root_));
}

TEST_F(WriteOperationsTest, AlterationPlanRetainsLeasedUploadsUntilApplyCompletes) {
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
    const auto planned = registry_.invoke("alter.plan", request, context());
    ASSERT_TRUE(planned) << planned.error().message;
    now_ += std::chrono::seconds{6};
    uploads_->cleanup();
    EXPECT_TRUE(uploads_->inspect(audio->reference, "owner"));
    EXPECT_TRUE(uploads_->inspect(manifest->reference, "owner"));

    const auto applied =
        registry_.invoke("alter.hds", {{"planToken", planned->at("planToken").get<std::string>()}}, context());
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

TEST_F(WriteOperationsTest, AlterationPlansReserveAndReleaseTheirDestination) {
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
    const auto first = registry_.invoke("alter.plan", request, context());
    ASSERT_TRUE(first) << first.error().message;
    const auto reserved = registry_.invoke("alter.plan", request, context());
    ASSERT_FALSE(reserved);
    EXPECT_EQ(reserved.error().code, "destination_reserved");

    const auto applied =
        registry_.invoke("alter.hds", {{"planToken", first->at("planToken").get<std::string>()}}, context());
    ASSERT_TRUE(applied) << applied.error().message;
    std::filesystem::remove(root_ / "reserved-alteration.hds");
    const auto released = registry_.invoke("alter.plan", request, context());
    ASSERT_TRUE(released) << released.error().message;
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
