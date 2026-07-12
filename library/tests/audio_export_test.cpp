#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <gtest/gtest.h>

#include "axklib/audio_export.hpp"

namespace {

std::filesystem::path fixture() {
  return std::filesystem::path{AXK_SOURCE_ROOT} /
         "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds";
}

} // namespace

TEST(AudioExport, BuildsExactVolumeOwnershipAndWritesEveryPhysicalWaveform) {
  const auto container = axk::open_image(fixture());
  ASSERT_TRUE(container);
  const auto catalog = axk::build_object_catalog(*container);
  ASSERT_TRUE(catalog);
  const auto graph = axk::build_relationship_graph(*catalog);
  const auto plan = axk::build_export_plan(*container, *catalog, graph);
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->volumes.size(), 1U);
  const auto &volume = plan->volumes[0];
  EXPECT_EQ(volume.relative_root, "partition_00_New_Partition/New Volume");
  EXPECT_EQ(volume.waveforms.size(), 8U);
  EXPECT_EQ(volume.sample_banks.size(), 8U);
  EXPECT_EQ(volume.sample_bank_groups.size(), 1U);
  EXPECT_TRUE(std::ranges::all_of(volume.sample_banks, [](const auto &bank) {
    return bank.members.size() == 1U && !bank.rendered_wav_path;
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
  EXPECT_TRUE(
      std::filesystem::is_regular_file(output / volume.relative_root / "B New SmpBank.sfz"));
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
  axk::VolumeExport volume;
  volume.relative_root = "partition_00_hd1/Vol 1";
  volume.waveforms = {
      {"left", "Left", "SMPL/Left.wav", left},
      {"right", "Right", "SMPL/Right.wav", right},
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
  volume.sample_banks.push_back(bank);
  volume.sample_bank_groups.push_back({"group", "Bank", {"bank"}});
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
  EXPECT_NE(text.find("lokey=48 hikey=72 pitch_keycenter=60"), std::string::npos);
  EXPECT_NE(text.find("transpose=1 tune=3"), std::string::npos);
  EXPECT_NE(text.find("loop_start=10 loop_end=89"), std::string::npos);
  EXPECT_EQ(text.find("SMPL/Left.wav"), std::string::npos);
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
