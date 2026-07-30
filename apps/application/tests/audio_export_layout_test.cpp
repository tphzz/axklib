#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "audio_export_layout.hpp"

namespace {

axk::Waveform waveform() {
    axk::Waveform result;
    result.format = {.channels = 1U, .sample_width_bytes = 2U, .sample_rate = 44'100U};
    result.frame_count = 2U;
    result.pcm = {std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}};
    return result;
}

axk::VolumeExport volume(std::string name, std::filesystem::path root, std::string waveform_key) {
    axk::VolumeExport result;
    result.volume_name = std::move(name);
    result.relative_root = std::move(root);
    result.waveforms.push_back({waveform_key, "Shared Wave", "SMPL/Shared Wave.wav", waveform()});
    axk::SampleExport sample;
    sample.object_key = "sample-" + waveform_key;
    sample.display_name = "Playable";
    sample.members.push_back({"mono", waveform_key, "SMPL/Shared Wave.wav", axk::RelationshipQuality::known});
    result.samples.push_back(std::move(sample));
    return result;
}

} // namespace

TEST(AudioExportLayout, SanitizesDisplayNamesConsistentlyAcrossExportEntryPoints) {
    EXPECT_EQ(axk::app::safe_audio_export_path_name("  Less < Greater > **  ", "sample"), "Less _lt_ Greater _gt (3)");
    EXPECT_EQ(axk::app::safe_audio_export_path_name("  ", "sample"), "sample");
}

TEST(AudioExportLayout, FlattensOneSelectedVolumeIntoTheOwnedDestinationAndPoolsWaveData) {
    axk::ExportPlan plan;
    plan.volumes.push_back(volume("FAT root", "FAT root", "wave"));
    axk::app::PooledPathAllocator pooled;

    const auto applied = axk::app::apply_audio_export_layout(plan, {{}, false, true}, pooled);

    ASSERT_TRUE(applied) << applied.error().message;
    const auto &result = plan.volumes.front();
    EXPECT_TRUE(result.relative_root.empty());
    EXPECT_EQ(result.waveforms.front().relative_wav_path.parent_path().generic_string(), "_samples/physical");
    EXPECT_EQ(result.samples.front().members.front().relative_wav_path, result.waveforms.front().relative_wav_path);
}

TEST(AudioExportLayout, PreservedVolumeViewsShareOneContentAddressedPool) {
    axk::ExportPlan plan;
    plan.volumes.push_back(volume("First", "partition_00/First", "first-wave"));
    plan.volumes.push_back(volume("Second", "partition_00/Second", "second-wave"));
    axk::app::PooledPathAllocator pooled;

    const auto applied = axk::app::apply_audio_export_layout(plan, {{}, true, true}, pooled);

    ASSERT_TRUE(applied) << applied.error().message;
    const auto first =
        (plan.volumes[0].relative_root / plan.volumes[0].waveforms[0].relative_wav_path).lexically_normal();
    const auto second =
        (plan.volumes[1].relative_root / plan.volumes[1].waveforms[0].relative_wav_path).lexically_normal();
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.parent_path().generic_string(), "_samples/physical");
}
