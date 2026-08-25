#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio_export.hpp"

namespace {

std::filesystem::path fixture() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored/"
                                                    "HD00_512_single_sbnk_authored.hds";
}

std::uint32_t wav_le32(std::span<const char> bytes, std::size_t offset) {
    std::uint32_t result{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8U);
    }
    return result;
}

std::size_t wav_chunk_offset(std::span<const char> bytes, std::string_view id) {
    for (std::size_t offset = 12U; offset + 8U <= bytes.size();) {
        if (id.size() == 4U && std::equal(id.begin(), id.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)))
            return offset;
        const auto payload_size = static_cast<std::size_t>(wav_le32(bytes, offset + 4U));
        const auto padded_size = payload_size + payload_size % 2U;
        if (padded_size > bytes.size() - offset - 8U)
            break;
        offset += 8U + padded_size;
    }
    return bytes.size();
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
    EXPECT_EQ(volume.samples.size(), 8U);
    EXPECT_EQ(volume.sample_banks.size(), 1U);
    EXPECT_TRUE(volume.sample_banks.front().member_sample_keys.empty());
    EXPECT_FALSE(volume.sample_banks.front().relationship_sample_keys.empty());
    EXPECT_TRUE(std::ranges::all_of(volume.samples, [](const auto &sample) {
        return sample.members.size() == 1U && !sample.rendered_wav_path && !sample.parameter_contexts.empty() &&
               sample.parameter_contexts.front().object_key == sample.object_key;
    }));
    EXPECT_TRUE(std::ranges::all_of(volume.waveforms, [](const auto &waveform) {
        return waveform.user_facing_aliases.size() == 1U &&
               waveform.user_facing_aliases.front().relationship_quality == axk::RelationshipQuality::known &&
               !waveform.user_facing_aliases.front().sample_object_key.empty() &&
               !waveform.user_facing_aliases.front().display_name.empty();
    }));

    const auto output = std::filesystem::temp_directory_path() / "axklib-cpp-export-test";
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
    EXPECT_FALSE(std::filesystem::is_regular_file(output / volume.relative_root / "B New SmpBank.sfz"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, WritesMissingAndAmbiguousWaveDataIntoExplicitUnresolvedScope) {
    const auto container = axk::open_image(fixture());
    ASSERT_TRUE(container);
    auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    auto wave_data = catalog->objects | std::views::filter([](const auto &item) {
                         return item.object.header.type == axk::ObjectType::smpl;
                     });
    auto first = wave_data.begin();
    ASSERT_NE(first, wave_data.end());
    first->placement.reset();
    first->placement_candidates.clear();
    first->placement_resolution = axk::PlacementResolution::missing;
    ++first;
    ASSERT_NE(first, wave_data.end());
    ASSERT_TRUE(first->placement);
    auto alternate = *first->placement;
    alternate.volume_name = "Other candidate";
    first->placement.reset();
    first->placement_candidates.push_back(std::move(alternate));
    first->placement_resolution = axk::PlacementResolution::ambiguous;

    const auto graph = axk::build_relationship_graph(*catalog);
    const auto plan = axk::build_export_plan(*container, *catalog, graph);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->volumes.size(), 1U);
    EXPECT_EQ(plan->volumes.front().waveforms.size(), 6U);
    ASSERT_EQ(plan->unresolved_wave_data.size(), 1U);
    const auto &unresolved = plan->unresolved_wave_data.front();
    EXPECT_EQ(unresolved.relative_root, "partition_00_New_Partition/Unresolved Wave Data");
    ASSERT_EQ(unresolved.waveforms.size(), 2U);
    EXPECT_EQ(unresolved.waveforms[0].placement_resolution, axk::PlacementResolution::missing);
    EXPECT_TRUE(unresolved.waveforms[0].placement_candidates.empty());
    EXPECT_EQ(unresolved.waveforms[0].user_facing_aliases.size(), 1U);
    EXPECT_EQ(unresolved.waveforms[1].placement_resolution, axk::PlacementResolution::ambiguous);
    EXPECT_EQ(unresolved.waveforms[1].placement_candidates.size(), 2U);
    EXPECT_EQ(unresolved.waveforms[1].user_facing_aliases.size(), 1U);

    const auto output = std::filesystem::temp_directory_path() / "axklib-cpp-unresolved-export-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto exported = axk::write_export_audio(*plan, output);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported->written_files.size(), 8U);
    EXPECT_TRUE(std::filesystem::is_regular_file(output / unresolved.relative_root /
                                                 unresolved.waveforms.front().relative_wav_path));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, DoesNotProjectLogicalSamplesAcrossVolumesWithoutALocalNameTarget) {
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
        std::ranges::find_if(plan->volumes, [](const auto &volume) { return !volume.waveforms.empty(); });
    const auto storage_volume =
        std::ranges::find(plan->volumes, "Storage-only volume", &axk::VolumeExport::volume_name);
    ASSERT_NE(waveform_volume, plan->volumes.end());
    ASSERT_NE(storage_volume, plan->volumes.end());
    EXPECT_TRUE(waveform_volume->samples.empty());
    EXPECT_TRUE(waveform_volume->sample_banks.empty());
    EXPECT_EQ(storage_volume->samples.size(), 8U);
    EXPECT_EQ(storage_volume->sample_banks.size(), 1U);
}

TEST(AudioExport, RetainsLikelyMembersAsGraphMetadataWithoutWritingSfzRegions) {
    const auto container = axk::open_image(fixture());
    ASSERT_TRUE(container);
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    auto graph = axk::build_relationship_graph(*catalog);
    for (auto &relationship : graph.relationships) {
        if (relationship.type == "SBNK_LEFT_MEMBER_TO_SMPL" || relationship.type == "SBNK_RIGHT_MEMBER_TO_SMPL") {
            relationship.quality = axk::RelationshipQuality::likely;
        }
    }
    const auto plan = axk::build_export_plan(*container, *catalog, graph);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->volumes.size(), 1U);
    EXPECT_TRUE(std::ranges::all_of(plan->volumes.front().samples, [](const auto &sample) {
        return sample.members.size() == 1U && sample.members.front().quality == axk::RelationshipQuality::likely &&
               !sample.rendered_wav_path;
    }));
    ASSERT_EQ(plan->volumes.front().sample_banks.size(), 1U);
    EXPECT_TRUE(plan->volumes.front().sample_banks.front().member_sample_keys.empty());
    EXPECT_FALSE(plan->volumes.front().sample_banks.front().relationship_sample_keys.empty());

    const auto output = std::filesystem::temp_directory_path() / "axklib-cpp-likely-sfz-test";
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
    left.frame_count = 100;
    left.root_key = 60;
    left.fine_tune_cents = 3;
    left.loop_mode = 1;
    left.loop_mode_label = "Forward";
    left.loop_start = 10;
    left.loop_length = 80;
    left.pcm.resize(200U);
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
    axk::SampleExport sample;
    sample.object_key = "sample";
    sample.display_name = "Stereo Member";
    sample.members = {
        {"left", "left", "SMPL/Left.wav", axk::RelationshipQuality::known},
        {"right", "right", "SMPL/Right.wav", axk::RelationshipQuality::known},
    };
    sample.rendered_wav_path = "RENDERED/Stereo Member.wav";
    sample.key_low = 48;
    sample.key_high = 72;
    sample.coarse_tune = 1;
    sample.decoded.left.root_key = 61;
    sample.decoded.left.fine_tune_cents = 4;
    sample.decoded.left.wave_length_frames = 100;
    sample.decoded.left.loop_start_frame = 10;
    sample.decoded.left.loop_length_frames = 80;
    sample.decoded.loop_mode = 1;
    sample.decoded.right = sample.decoded.left;
    auto invalid_tune = sample;
    invalid_tune.object_key = "invalid-tune";
    invalid_tune.display_name = "Invalid Tune";
    invalid_tune.members.resize(1);
    invalid_tune.members[0].waveform_key = "wide-loop";
    invalid_tune.members[0].relative_wav_path = "SMPL/Wide Loop.wav";
    invalid_tune.rendered_wav_path.reset();
    invalid_tune.coarse_tune = 113;
    auto duplicate_name = invalid_tune;
    duplicate_name.object_key = "duplicate-name";
    volume.samples = {sample, invalid_tune, duplicate_name};
    volume.sample_banks.push_back({"sample_bank", " Bank", {"sample"}, {"sample"}});
    axk::ExportPlan plan;
    plan.volumes.push_back(std::move(volume));

    const auto output = std::filesystem::temp_directory_path() / "axklib-cpp-sfz-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    ASSERT_TRUE(axk::write_export_audio(plan, output));
    const auto result = axk::write_sfz(plan, output);
    ASSERT_TRUE(result);
    const auto path = output / "partition_00_hd1/Vol 1/B Bank.sfz";
    std::ifstream input{path};
    const std::string text{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_NE(text.find("sample=RENDERED/Stereo Member.wav"), std::string::npos);
    EXPECT_NE(text.find("lokey=48 hikey=72 pitch_keycenter=61"), std::string::npos);
    EXPECT_NE(text.find("transpose=1 tune=4"), std::string::npos);
    EXPECT_NE(text.find("loop_start=10 loop_end=89"), std::string::npos);
    EXPECT_EQ(text.find("SMPL/Left.wav"), std::string::npos);
    std::ifstream invalid_input{output / "partition_00_hd1/Vol 1/Invalid Tune.sfz"};
    const std::string invalid_text{std::istreambuf_iterator<char>{invalid_input}, {}};
    EXPECT_EQ(invalid_text.find("transpose="), std::string::npos);
    EXPECT_NE(invalid_text.find("tune=4"), std::string::npos);
    EXPECT_NE(invalid_text.find("loop_start=10 loop_end=89"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "partition_00_hd1/Vol 1/Invalid Tune (2).sfz"));
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
    const auto output = std::filesystem::temp_directory_path() / "axklib-cpp-preflight-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto existing = output / "partition_00_hd1/Volume/SMPL/Two.wav";
    std::filesystem::create_directories(existing.parent_path());
    std::ofstream{existing} << "retained";

    EXPECT_FALSE(axk::write_export_audio(plan, output));
    EXPECT_FALSE(std::filesystem::exists(output / "partition_00_hd1/Volume/SMPL/One.wav"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, WritesSelectedSampleAsOneFlatInterleavedStereoWav) {
    axk::Waveform left;
    left.format = {1, 2, 44'100};
    left.frame_count = 4;
    left.pcm = {std::byte{1}, std::byte{0}, std::byte{2}, std::byte{0},
                std::byte{5}, std::byte{0}, std::byte{6}, std::byte{0}};
    auto right = left;
    right.pcm = {std::byte{3}, std::byte{0}, std::byte{4}, std::byte{0},
                 std::byte{7}, std::byte{0}, std::byte{8}, std::byte{0}};

    axk::SampleExport sample;
    sample.object_key = "sample";
    sample.display_name = "Stereo Sample";
    sample.key_low = 36U;
    sample.key_high = 84U;
    sample.decoded.velocity_range_low = 12U;
    sample.decoded.velocity_range_high = 110U;
    sample.decoded.loop_mode = 1U;
    sample.decoded.left.root_key = 60U;
    sample.decoded.left.fine_tune_cents = -25;
    sample.decoded.left.wave_length_frames = 4U;
    sample.decoded.left.loop_start_frame = 1U;
    sample.decoded.left.loop_length_frames = 2U;
    sample.decoded.right = sample.decoded.left;
    sample.members = {
        {"left", "left", "SMPL/Left.wav", axk::RelationshipQuality::known},
        {"right", "right", "SMPL/Right.wav", axk::RelationshipQuality::known},
    };
    axk::VolumeExport volume;
    volume.waveforms = {
        {"left", "Left", "SMPL/Left.wav", left},
        {"right", "Right", "SMPL/Right.wav", right},
    };
    volume.samples = {sample};
    axk::ExportPlan plan;
    plan.volumes = {volume};

    const auto output = std::filesystem::temp_directory_path() / "axklib-selected-stereo-wav-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto result = axk::write_selected_wav_audio(plan, {axk::SelectedWavExportKind::sample, {"sample"}}, output);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->written_files.size(), 1U);
    EXPECT_EQ(result->written_files.front().filename(), "Stereo Sample.wav");

    std::ifstream input{result->written_files.front(), std::ios::binary};
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input}, {}};
    const auto smpl = wav_chunk_offset(bytes, "smpl");
    const auto inst = wav_chunk_offset(bytes, "inst");
    const auto data = wav_chunk_offset(bytes, "data");
    ASSERT_LT(smpl, bytes.size());
    ASSERT_LT(inst, bytes.size());
    ASSERT_LT(data, bytes.size());
    EXPECT_EQ(static_cast<unsigned char>(bytes[22]), 2U);
    EXPECT_EQ(wav_le32(bytes, smpl + 16U), 22'675U);
    EXPECT_EQ(wav_le32(bytes, smpl + 36U), 1U);
    EXPECT_EQ(wav_le32(bytes, smpl + 52U), 1U);
    EXPECT_EQ(wav_le32(bytes, smpl + 56U), 2U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[inst + 8U]), 60U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[inst + 11U]), 36U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[inst + 12U]), 84U);
    EXPECT_EQ(wav_le32(bytes, data + 4U), 16U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[data + 8U]), 1U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[data + 10U]), 3U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[data + 12U]), 2U);
    EXPECT_EQ(static_cast<unsigned char>(bytes[data + 14U]), 4U);
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, WritesOnlyTheSelectedWaveDataAndUsesDeterministicNameSuffixes) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44'100};
    waveform.frame_count = 1;
    waveform.pcm = {std::byte{}, std::byte{}};
    axk::VolumeExport volume;
    volume.waveforms = {
        {"first", "Same", "SMPL/First.wav", waveform},
        {"second", "Same", "SMPL/Second.wav", waveform},
        {"excluded", "Excluded", "SMPL/Excluded.wav", waveform},
    };
    axk::ExportPlan plan;
    plan.volumes = {volume};

    const auto output = std::filesystem::temp_directory_path() / "axklib-selected-wave-data-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto result =
        axk::write_selected_wav_audio(plan, {axk::SelectedWavExportKind::wave_data, {"first", "second"}}, output);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->written_files.size(), 2U);
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "Same.wav"));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "Same (2).wav"));
    EXPECT_FALSE(std::filesystem::exists(output / "Excluded.wav"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, RejectsSelectedSampleWithoutExactWaveDataMembersBeforeWriting) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44'100};
    waveform.frame_count = 1;
    waveform.pcm = {std::byte{}, std::byte{}};
    axk::SampleExport sample;
    sample.object_key = "sample";
    sample.display_name = "Tentative Sample";
    sample.members = {{"left", "wave", "SMPL/Wave.wav", axk::RelationshipQuality::likely}};
    axk::VolumeExport volume;
    volume.waveforms = {{"wave", "Wave", "SMPL/Wave.wav", waveform}};
    volume.samples = {sample};
    axk::ExportPlan plan;
    plan.volumes = {volume};

    const auto output = std::filesystem::temp_directory_path() / "axklib-selected-invalid-sample-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto result = axk::write_selected_wav_audio(plan, {axk::SelectedWavExportKind::sample, {"sample"}}, output);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::relationship_unresolved);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(AudioExport, WritesSharedIdenticalTargetOnceAndRejectsDistinctCollisionBeforeOutput) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44100};
    waveform.frame_count = 1;
    waveform.pcm = {std::byte{}, std::byte{}};
    axk::VolumeExport first;
    first.relative_root = "file/source/volume";
    first.waveforms = {{"one", "Shared", "../../../_samples/physical/shared.wav", waveform}};
    auto second = first;
    second.relative_root = "file/source/other-volume";
    second.waveforms[0].object_key = "two";
    axk::ExportPlan plan;
    plan.volumes = {first, second};
    const auto output = std::filesystem::temp_directory_path() / "axklib-cpp-shared-target-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);

    const auto shared = axk::write_export_audio(plan, output);
    ASSERT_TRUE(shared);
    ASSERT_EQ(shared->written_files.size(), 1U);
    EXPECT_TRUE(std::filesystem::is_regular_file(shared->written_files.front()));

    std::filesystem::remove_all(output, error);
    plan.volumes[1].waveforms[0].waveform.pcm[0] = std::byte{1};
    const auto collision = axk::write_export_audio(plan, output);
    ASSERT_FALSE(collision);
    EXPECT_NE(collision.error().message.find("distinct audio exports"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output / "_samples/physical/shared.wav"));
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
    axk::SampleExport first_sample;
    first_sample.object_key = "sample-one";
    first_sample.display_name = "Duplicate";
    first_sample.members = {{"left", "wave-one", "SMPL/Wave.wav", axk::RelationshipQuality::known}};
    first_sample.decoded.left.wave_length_frames = 1;
    first.samples = {first_sample};
    auto second = first;
    second.waveforms[0].object_key = "wave-two";
    second.samples[0].object_key = "sample-two";
    second.samples[0].members[0].waveform_key = "wave-two";
    axk::ExportPlan plan;
    plan.volumes = {first, second};
    const auto output = std::filesystem::temp_directory_path() / "axklib-cpp-sfz-dedupe-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);

    const auto result = axk::write_sfz(plan, output);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->written_files.size(), 2U);
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "shared-volume/Duplicate.sfz"));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "shared-volume/Duplicate (2).sfz"));
    std::filesystem::remove_all(output, error);
}

TEST(AudioExport, UsesEachSamplesPlaybackWindowAndLoopPolicyForSharedWaveData) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44'100};
    waveform.frame_count = 100;
    waveform.loop_mode = 1;
    waveform.loop_mode_label = "Forward";
    waveform.loop_start = 5;
    waveform.loop_length = 90;
    waveform.pcm.resize(200U);

    axk::SampleExport first;
    first.object_key = "first";
    first.display_name = "First";
    first.members = {{"left", "shared", "SMPL/Shared.wav", axk::RelationshipQuality::known}};
    first.decoded.left.wave_start_frame = 20;
    first.decoded.left.wave_length_frames = 40;
    first.decoded.left.loop_start_frame = 25;
    first.decoded.left.loop_length_frames = 10;
    first.decoded.loop_mode = 0;

    auto second = first;
    second.object_key = "second";
    second.display_name = "Second";
    second.decoded.left.wave_start_frame = 60;
    second.decoded.left.wave_length_frames = 20;
    second.decoded.left.loop_start_frame = 65;
    second.decoded.left.loop_length_frames = 5;
    second.decoded.loop_mode = 2;

    axk::VolumeExport volume;
    volume.relative_root = "volume";
    volume.waveforms = {{"shared", "Shared", "SMPL/Shared.wav", waveform}};
    volume.samples = {first, second};
    axk::ExportPlan plan;
    plan.volumes = {volume};

    const auto output = std::filesystem::temp_directory_path() / "axklib-sfz-sample-window-test";
    std::error_code error;
    std::filesystem::remove_all(output, error);
    const auto result = axk::write_sfz(plan, output);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->warnings.size(), 1U);
    EXPECT_NE(result->warnings.front().find("release-tail looping"), std::string::npos);
    std::ifstream first_input{output / "volume/First.sfz"};
    const std::string first_text{std::istreambuf_iterator<char>{first_input}, {}};
    EXPECT_NE(first_text.find("offset=20 end=59 loop_mode=one_shot"), std::string::npos);
    EXPECT_EQ(first_text.find("loop_start="), std::string::npos);
    std::ifstream second_input{output / "volume/Second.sfz"};
    const std::string second_text{std::istreambuf_iterator<char>{second_input}, {}};
    EXPECT_NE(second_text.find("offset=60 end=79 loop_mode=loop_continuous loop_start=65 loop_end=69"),
              std::string::npos);
    first_input.close();
    second_input.close();

    plan.volumes.front().samples.front().decoded.left.wave_length_frames = 81;
    std::filesystem::remove_all(output, error);
    const auto invalid = axk::write_sfz(plan, output);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, axk::ErrorCode::object_malformed);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(AudioExport, RejectsEveryEscapingPlanPathBeforeCreatingOutput) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44100};
    waveform.frame_count = 1;
    waveform.pcm = {std::byte{}, std::byte{}};
    axk::VolumeExport volume;
    volume.relative_root = "volume";
    volume.waveforms = {{"wave", "Wave", "SMPL/Wave.wav", waveform}};
    axk::SampleExport sample;
    sample.object_key = "sample";
    sample.display_name = "Sample";
    sample.members = {{"left", "wave", "SMPL/Wave.wav", axk::RelationshipQuality::known}};
    sample.rendered_wav_path = "RENDERED/Sample.wav";
    volume.samples = {sample};
    axk::ExportPlan base;
    base.volumes = {volume};
    base.unresolved_wave_data = {{axk::PartitionIndex{0}, "", "unresolved", {volume.waveforms.front()}}};

    const auto root = std::filesystem::temp_directory_path() / "axklib-export-containment-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto outside = root.parent_path() / "axklib-export-containment-outside.wav";
    std::filesystem::remove(outside, error);

    const auto rejected = [&](axk::ExportPlan plan, bool sfz = false) {
        const auto result = sfz ? static_cast<bool>(axk::write_sfz(plan, root))
                                : static_cast<bool>(axk::write_export_audio(plan, root));
        EXPECT_FALSE(result);
        EXPECT_FALSE(std::filesystem::exists(root));
        EXPECT_FALSE(std::filesystem::exists(outside));
    };

    auto absolute = base;
    absolute.volumes[0].waveforms[0].relative_wav_path = outside;
    rejected(std::move(absolute));

    auto physical = base;
    physical.volumes[0].waveforms[0].relative_wav_path = "../../axklib-export-containment-outside.wav";
    rejected(std::move(physical));

    auto rendered = base;
    rendered.volumes[0].samples[0].rendered_wav_path = "../../axklib-export-containment-outside.wav";
    rejected(std::move(rendered));

    auto member = base;
    member.volumes[0].samples[0].members[0].relative_wav_path = "../../axklib-export-containment-outside.wav";
    rejected(std::move(member), true);

    auto unresolved = base;
    unresolved.unresolved_wave_data[0].relative_root = "../../escape";
    rejected(std::move(unresolved));

    auto sfz = base;
    sfz.volumes[0].relative_root = "../../escape";
    rejected(std::move(sfz), true);
}
