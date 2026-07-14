#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <gtest/gtest.h>

#include "axklib/audio_export.hpp"

namespace {

std::filesystem::path fixture() {
    return std::filesystem::path{AXK_SOURCE_ROOT} /
           "tests/fixtures/images/sampler-authored/"
           "HD00_512_single_sbnk_authored.hds";
}

} // namespace

TEST(AudioExport, BuildsExactVolumeOwnershipAndWritesEveryPhysicalWaveform) {
    const auto container = axk::open_image(fixture());
    ASSERT_TRUE(container);
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    auto graph = axk::build_relationship_graph(*catalog);
    for (auto &relationship : graph.relationships) {
        if (relationship.type == "SBAC_SLOT_TO_SBNK")
            relationship.quality = axk::RelationshipQuality::likely;
    }
    const auto plan = axk::build_export_plan(*container, *catalog, graph);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->volumes.size(), 1U);
    const auto &volume = plan->volumes[0];
    EXPECT_EQ(volume.relative_root, "partition_00_New_Partition/New Volume");
    EXPECT_EQ(volume.waveforms.size(), 8U);
    EXPECT_EQ(volume.sample_banks.size(), 8U);
    EXPECT_EQ(volume.sample_bank_groups.size(), 1U);
    EXPECT_TRUE(volume.sample_bank_groups.front().member_bank_keys.empty());
    EXPECT_FALSE(
        volume.sample_bank_groups.front().relationship_bank_keys.empty());
    EXPECT_TRUE(std::ranges::all_of(volume.sample_banks, [](const auto &bank) {
        return bank.members.size() == 1U && !bank.rendered_wav_path &&
               !bank.parameter_contexts.empty() &&
               bank.parameter_contexts.front().object_key == bank.object_key;
    }));

    const auto output =
        std::filesystem::temp_directory_path() / "axklib-cpp-export-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto exported = axk::write_export_audio(*plan, output);
    ASSERT_TRUE(exported);
    EXPECT_EQ(exported->written_files.size(), 8U);
    for (const auto &path : exported->written_files)
        EXPECT_TRUE(std::filesystem::is_regular_file(path));
    const auto sfz = axk::write_sfz(*plan, output);
    ASSERT_TRUE(sfz);
    EXPECT_EQ(sfz->written_files.size(), 8U);
    EXPECT_FALSE(std::filesystem::is_regular_file(
        output / volume.relative_root / "B New SmpBank.sfz"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, ProjectsLogicalBanksToTheirKnownWaveformVolume) {
    const auto container = axk::open_image(fixture());
    ASSERT_TRUE(container);
    auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    for (auto &item : catalog->objects) {
        if (!item.placement || item.object.header.type == axk::ObjectType::smpl)
            continue;
        item.placement->volume_directory = axk::SfsId{999U};
        item.placement->volume_name = "Storage-only volume";
    }
    const auto graph = axk::build_relationship_graph(*catalog);
    const auto plan = axk::build_export_plan(*container, *catalog, graph);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->volumes.size(), 2U);
    const auto waveform_volume =
        std::ranges::find_if(plan->volumes, [](const auto &volume) {
            return !volume.waveforms.empty();
        });
    const auto storage_volume = std::ranges::find(
        plan->volumes, "Storage-only volume", &axk::VolumeExport::volume_name);
    ASSERT_NE(waveform_volume, plan->volumes.end());
    ASSERT_NE(storage_volume, plan->volumes.end());
    EXPECT_EQ(waveform_volume->sample_banks.size(), 8U);
    EXPECT_EQ(waveform_volume->sample_bank_groups.size(), 1U);
    EXPECT_TRUE(storage_volume->sample_banks.empty());
    EXPECT_TRUE(storage_volume->sample_bank_groups.empty());
}

TEST(AudioExport, RetainsLikelyMembersAsGraphMetadataWithoutWritingSfzRegions) {
    const auto container = axk::open_image(fixture());
    ASSERT_TRUE(container);
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    auto graph = axk::build_relationship_graph(*catalog);
    for (auto &relationship : graph.relationships) {
        if (relationship.type == "SBNK_LEFT_MEMBER_TO_SMPL" ||
            relationship.type == "SBNK_RIGHT_MEMBER_TO_SMPL") {
            relationship.quality = axk::RelationshipQuality::likely;
        }
    }
    const auto plan = axk::build_export_plan(*container, *catalog, graph);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->volumes.size(), 1U);
    EXPECT_TRUE(std::ranges::all_of(
        plan->volumes.front().sample_banks, [](const auto &bank) {
            return bank.members.size() == 1U &&
                   bank.members.front().quality ==
                       axk::RelationshipQuality::likely &&
                   !bank.rendered_wav_path;
        }));
    ASSERT_EQ(plan->volumes.front().sample_bank_groups.size(), 1U);
    EXPECT_TRUE(plan->volumes.front()
                    .sample_bank_groups.front()
                    .member_bank_keys.empty());
    EXPECT_FALSE(plan->volumes.front()
                     .sample_bank_groups.front()
                     .relationship_bank_keys.empty());

    const auto output =
        std::filesystem::temp_directory_path() / "axklib-cpp-likely-sfz-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto sfz = axk::write_sfz(*plan, output);
    ASSERT_TRUE(sfz);
    EXPECT_TRUE(sfz->written_files.empty());
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, PrefersRenderedStereoAndEmitsSamplerParametersToSfz) {
    axk::Waveform left;
    left.object_key = "left";
    left.name = "Left";
    left.format = {1, 2, 44100};
    left.frame_count = 2;
    left.root_key = 60;
    left.fine_tune_cents = 3;
    left.loop_mode = 1;
    left.loop_mode_label = "Forward";
    left.loop_start = 10;
    left.loop_length = 80;
    left.pcm = {std::byte{}, std::byte{}, std::byte{}, std::byte{}};
    auto right = left;
    right.object_key = "right";
    right.name = "Right";
    auto wide_loop = left;
    wide_loop.object_key = "wide-loop";
    wide_loop.name = "Wide Loop";
    wide_loop.loop_start = 23'423U;
    wide_loop.loop_length = 4'294'967'293U;
    axk::VolumeExport volume;
    volume.relative_root = "partition_00_hd1/Vol 1";
    volume.waveforms = {
        {"left", "Left", "SMPL/Left.wav", left},
        {"right", "Right", "SMPL/Right.wav", right},
        {"wide-loop", "Wide Loop", "SMPL/Wide Loop.wav", wide_loop},
    };
    axk::SampleBankExport bank;
    bank.object_key = "bank";
    bank.display_name = "Stereo Member";
    bank.members = {
        {"left", "left", "SMPL/Left.wav", axk::RelationshipQuality::known},
        {"right", "right", "SMPL/Right.wav", axk::RelationshipQuality::known},
    };
    bank.rendered_wav_path = "RENDERED/Stereo Member.wav";
    bank.key_low = 48;
    bank.key_high = 72;
    bank.coarse_tune = 1;
    bank.decoded.left.root_key = 61;
    bank.decoded.left.fine_tune_cents = 4;
    auto invalid_tune = bank;
    invalid_tune.object_key = "invalid-tune";
    invalid_tune.display_name = "Invalid Tune";
    invalid_tune.members.resize(1);
    invalid_tune.members[0].waveform_key = "wide-loop";
    invalid_tune.members[0].relative_wav_path = "SMPL/Wide Loop.wav";
    invalid_tune.rendered_wav_path.reset();
    invalid_tune.coarse_tune = 113;
    auto duplicate_name = invalid_tune;
    duplicate_name.object_key = "duplicate-name";
    volume.sample_banks = {bank, invalid_tune, duplicate_name};
    volume.sample_bank_groups.push_back({"group", " Bank", {"bank"}, {"bank"}});
    axk::ExportPlan plan;
    plan.volumes.push_back(std::move(volume));

    const auto output =
        std::filesystem::temp_directory_path() / "axklib-cpp-sfz-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    ASSERT_TRUE(axk::write_export_audio(plan, output));
    const auto result = axk::write_sfz(plan, output);
    ASSERT_TRUE(result);
    const auto path = output / "partition_00_hd1/Vol 1/B Bank.sfz";
    std::ifstream input{path};
    const std::string text{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_NE(text.find("sample=RENDERED/Stereo Member.wav"),
              std::string::npos);
    EXPECT_NE(text.find("lokey=48 hikey=72 pitch_keycenter=61"),
              std::string::npos);
    EXPECT_NE(text.find("transpose=1 tune=4"), std::string::npos);
    EXPECT_NE(text.find("loop_start=10 loop_end=89"), std::string::npos);
    EXPECT_EQ(text.find("SMPL/Left.wav"), std::string::npos);
    std::ifstream invalid_input{output /
                                "partition_00_hd1/Vol 1/Invalid Tune.sfz"};
    const std::string invalid_text{
        std::istreambuf_iterator<char>{invalid_input}, {}};
    EXPECT_EQ(invalid_text.find("transpose="), std::string::npos);
    EXPECT_NE(invalid_text.find("tune=4"), std::string::npos);
    EXPECT_NE(invalid_text.find("loop_start=23423 loop_end=4294990715"),
              std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        output / "partition_00_hd1/Vol 1/Invalid Tune (2).sfz"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, PreflightsExistingTargetsBeforeWritingAnyAudio) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44100};
    waveform.frame_count = 1;
    waveform.pcm = {std::byte{}, std::byte{}};
    axk::VolumeExport volume;
    volume.relative_root = "partition_00_hd1/Volume";
    volume.waveforms = {
        {"one", "One", "SMPL/One.wav", waveform},
        {"two", "Two", "SMPL/Two.wav", waveform},
    };
    axk::ExportPlan plan;
    plan.volumes.push_back(std::move(volume));
    const auto output =
        std::filesystem::temp_directory_path() / "axklib-cpp-preflight-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto existing = output / "partition_00_hd1/Volume/SMPL/Two.wav";
    std::filesystem::create_directories(existing.parent_path());
    std::ofstream{existing} << "retained";

    EXPECT_FALSE(axk::write_export_audio(plan, output));
    EXPECT_FALSE(std::filesystem::exists(
        output / "partition_00_hd1/Volume/SMPL/One.wav"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport,
     WritesSharedIdenticalTargetOnceAndRejectsDistinctCollisionBeforeOutput) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44100};
    waveform.frame_count = 1;
    waveform.pcm = {std::byte{}, std::byte{}};
    axk::VolumeExport first;
    first.relative_root = "file/source/volume";
    first.waveforms = {
        {"one", "Shared", "../../../_samples/physical/shared.wav", waveform}};
    auto second = first;
    second.relative_root = "file/source/other-volume";
    second.waveforms[0].object_key = "two";
    axk::ExportPlan plan;
    plan.volumes = {first, second};
    const auto output = std::filesystem::temp_directory_path() /
                        "axklib-cpp-shared-target-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);

    const auto shared = axk::write_export_audio(plan, output);
    ASSERT_TRUE(shared);
    ASSERT_EQ(shared->written_files.size(), 1U);
    EXPECT_TRUE(
        std::filesystem::is_regular_file(shared->written_files.front()));

    std::filesystem::remove_all(output, error);
    plan.volumes[1].waveforms[0].waveform.pcm[0] = std::byte{1};
    const auto collision = axk::write_export_audio(plan, output);
    ASSERT_FALSE(collision);
    EXPECT_NE(collision.error().message.find("distinct audio exports"),
              std::string::npos);
    EXPECT_FALSE(
        std::filesystem::exists(output / "_samples/physical/shared.wav"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, DeduplicatesSfzNamesAcrossLogicalVolumesSharingOneDirectory) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44100};
    waveform.frame_count = 1;
    waveform.pcm = {std::byte{}, std::byte{}};
    axk::VolumeExport first;
    first.relative_root = "shared-volume";
    first.waveforms = {{"wave-one", "Wave", "SMPL/Wave.wav", waveform}};
    axk::SampleBankExport first_bank;
    first_bank.object_key = "bank-one";
    first_bank.display_name = "Duplicate";
    first_bank.members = {
        {"left", "wave-one", "SMPL/Wave.wav", axk::RelationshipQuality::known}};
    first.sample_banks = {first_bank};
    auto second = first;
    second.waveforms[0].object_key = "wave-two";
    second.sample_banks[0].object_key = "bank-two";
    second.sample_banks[0].members[0].waveform_key = "wave-two";
    axk::ExportPlan plan;
    plan.volumes = {first, second};
    const auto output =
        std::filesystem::temp_directory_path() / "axklib-cpp-sfz-dedupe-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);

    const auto result = axk::write_sfz(plan, output);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->written_files.size(), 2U);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        output / "shared-volume/Duplicate.sfz"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        output / "shared-volume/Duplicate (2).sfz"));
    std::filesystem::remove_all(output, error);
}
