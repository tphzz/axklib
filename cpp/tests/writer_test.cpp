#include <algorithm>
#include <fstream>

#if defined(__unix__)
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

constexpr std::string_view manifest = R"json({
  "schema_version":"1.0",
  "size_bytes":536870912,
  "partitions":[{
    "name":"P1",
    "volumes":[{
      "name":"V1",
      "waveforms":[{"id":"tone","name":"Tone","path":"audio/tone.wav","root_key":60}],
      "sample_banks":[{"name":"Tone Bank","waveform_id":"tone","root_key":60,"key_low":0,"key_high":127}]
    }]
  }]
})json";

} // namespace

TEST(HdsManifest, ParsesStrictSchemaAndResolvesRelativeAudioPaths) {
  const auto parsed = axk::parse_hds_build_manifest(manifest, "/project");
  ASSERT_TRUE(parsed) << parsed.error().message;
  ASSERT_EQ(parsed->partitions.size(), 1U);
  EXPECT_EQ(parsed->partitions[0].volumes[0].waveforms[0].path, "/project/audio/tone.wav");
  const auto geometry = axk::plan_hds_geometry(*parsed);
  ASSERT_TRUE(geometry);
  ASSERT_EQ(geometry->size(), 1U);
  EXPECT_EQ((*geometry)[0].start_sector, 3U);
  EXPECT_EQ((*geometry)[0].first_payload_cluster, 488U);
}

TEST(HdsManifest, RejectsUnknownFieldsReferencesAndInvalidGeometry) {
  auto unknown = std::string{manifest};
  unknown.replace(unknown.find("\"size_bytes\""), 12, "\"unknown\"");
  EXPECT_FALSE(axk::parse_hds_build_manifest(unknown));

  auto reference = std::string{manifest};
  reference.replace(reference.find("\"tone\",\"root_key\"", 200), 19, "\"missing\",\"root_key\"");
  EXPECT_FALSE(axk::parse_hds_build_manifest(reference));

  auto size = std::string{manifest};
  size.replace(size.find("536870912"), 9, "1048577");
  EXPECT_FALSE(axk::parse_hds_build_manifest(size));
}

TEST(HdsGeometry, CoversEveryPartitionCountAtOneAndTwoGiBBoundaries) {
  for (std::uint8_t count = 1; count <= 8; ++count) {
    axk::HdsBuildManifest value{
        "1.0", count == 1 ? axk::minimum_hds_size : axk::maximum_hds_size, {}};
    for (std::uint8_t index = 0; index < count; ++index) {
      axk::VolumeSpec volume;
      volume.name = "V";
      value.partitions.push_back({"P", {std::move(volume)}});
    }
    const auto geometry = axk::plan_hds_geometry(value);
    ASSERT_TRUE(geometry) << static_cast<int>(count);
    EXPECT_EQ(geometry->size(), count);
  }
}

TEST(AudioImport, PreservesNativePcm16AndChoosesOnlyHardwareRates) {
  axk::Waveform source;
  source.format = {1, 2, 48000};
  source.frame_count = 3;
  source.pcm = {std::byte{0x00}, std::byte{0x80}, std::byte{0x34},
                std::byte{0x12}, std::byte{0xff}, std::byte{0x7f}};
  const auto path = std::filesystem::temp_directory_path() / "axklib-writer-import.wav";
  std::error_code error;
  std::filesystem::remove(path, error);
  ASSERT_TRUE(axk::write_wav_atomic(path, source));
  axk::AudioImportOptions options;
  options.expected_channels = 1;
  const auto imported = axk::import_sampler_audio(path, options);
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_EQ(imported->source_format, "WAV");
  EXPECT_EQ(imported->source_subtype, "PCM_16");
  EXPECT_EQ(imported->output_frames, 3U);
  EXPECT_FALSE(imported->quantized);
  ASSERT_EQ(imported->pcm_channels.size(), 1U);
  EXPECT_EQ(imported->pcm_channels[0], source.pcm);
  EXPECT_EQ(*axk::choose_sampler_sample_rate(96000), 44100U);
  EXPECT_FALSE(axk::choose_sampler_sample_rate(48000, 47999));
  std::filesystem::remove(path, error);
}

TEST(HdsWriter, AtomicallyWritesAndReopensFreshEmptyVolumeImage) {
  axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {}};
  axk::VolumeSpec volume;
  volume.name = "Empty Volume";
  manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
  const auto path = std::filesystem::temp_directory_path() / "axklib-native-empty.hds";
  std::error_code error;
  std::filesystem::remove(path, error);
  const auto written = axk::write_hds_image(manifest_value, path);
  ASSERT_TRUE(written) << written.error().message;
  EXPECT_EQ(std::filesystem::file_size(path), axk::minimum_hds_size);
  EXPECT_FALSE(axk::write_hds_image(manifest_value, path));
  const auto reopened = axk::open_image(path);
  ASSERT_TRUE(reopened) << reopened.error().message;
  ASSERT_EQ(reopened->partitions().size(), 1U);
  EXPECT_EQ(reopened->partitions()[0].name, "hd1");
  std::filesystem::remove(path, error);
}

TEST(HdsWriter, WritesMonoSampleBankAndRoundTripsExactPhysicalPcm) {
  axk::Waveform source;
  source.format = {1, 2, 44100};
  source.frame_count = 3;
  source.pcm = {std::byte{},     std::byte{},     std::byte{0xe8},
                std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
  const auto audio_path = std::filesystem::temp_directory_path() / "axklib-writer-bank.wav";
  const auto image_path = std::filesystem::temp_directory_path() / "axklib-writer-bank.hds";
  std::error_code error;
  std::filesystem::remove(audio_path, error);
  std::filesystem::remove(image_path, error);
  ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));

  axk::WaveformSpec waveform{"wave", "Wave", audio_path, 60, {}};
  axk::SampleBankSpec bank;
  bank.name = "Bank";
  bank.waveform_id = "wave";
  bank.root_key = 60;
  bank.key_low = 48;
  bank.key_high = 72;
  bank.level = 96;
  axk::VolumeSpec volume;
  volume.name = "Volume";
  volume.waveforms.push_back(std::move(waveform));
  volume.sample_banks.push_back(std::move(bank));
  axk::HdsBuildManifest manifest_value{"1.0", 4U * 1024U * 1024U, {}};
  manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
  ASSERT_TRUE(axk::write_hds_image(manifest_value, image_path));
  const auto reopened = axk::open_image(image_path);
  ASSERT_TRUE(reopened);
  const auto catalog = axk::build_object_catalog(*reopened);
  ASSERT_TRUE(catalog);
  const auto type = [](const axk::ObjectSnapshot &item) { return item.object.header.type; };
  EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::smpl, type), 1U);
  EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::sbnk, type), 1U);
  const auto sample = std::ranges::find(catalog->objects, axk::ObjectType::smpl, type);
  ASSERT_NE(sample, catalog->objects.end());
  const auto decoded = axk::decode_waveform(*reopened, *sample);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(std::ranges::equal(
      source.pcm, std::span<const std::byte>{decoded->pcm}.first(source.pcm.size())));
  std::filesystem::remove(audio_path, error);
  std::filesystem::remove(image_path, error);
}

TEST(HdsWriter, CancellationPublishesNoImageOrTemporarySibling) {
  axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {}};
  axk::VolumeSpec volume;
  volume.name = "Volume";
  manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
  const auto path = std::filesystem::temp_directory_path() / "axklib-cancelled-writer.hds";
  const auto temporary = path.parent_path() / ("." + path.filename().string() + ".tmp");
  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::remove(temporary, error);
  axk::CancellationSource cancellation;
  cancellation.cancel();
  const auto written = axk::write_hds_image(manifest_value, path, false, cancellation.token());
  ASSERT_FALSE(written);
  EXPECT_EQ(written.error().code, axk::ErrorCode::operation_cancelled);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(temporary));
}

TEST(HdsWriter, OverwriteReplacesSymlinkWithoutFollowingItsTarget) {
  axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {}};
  axk::VolumeSpec volume;
  volume.name = "Volume";
  manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
  const auto root = std::filesystem::temp_directory_path() / "axklib-writer-symlink";
  const auto target = root / "target.txt";
  const auto output = root / "output.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  std::ofstream{target} << "sentinel";
  std::filesystem::create_symlink(target, output, error);
  if (error) {
    GTEST_SKIP() << "symbolic links are unavailable: " << error.message();
  }
  EXPECT_FALSE(axk::write_hds_image(manifest_value, output, false));
  const auto written = axk::write_hds_image(manifest_value, output, true);
  ASSERT_TRUE(written) << written.error().message;
  EXPECT_EQ(std::filesystem::file_size(target), 8U);
  EXPECT_FALSE(std::filesystem::is_symlink(output));
  EXPECT_EQ(std::filesystem::file_size(output), axk::minimum_hds_size);
  std::filesystem::remove_all(root, error);
}

#if defined(__unix__)
TEST(HdsWriter, DiskFullRemovesTemporaryOutputAndPublishesNothing) {
  axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {}};
  axk::VolumeSpec volume;
  volume.name = "Volume";
  manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
  const auto root = std::filesystem::temp_directory_path() /
                    ("axklib-writer-disk-full-" + std::to_string(::getpid()));
  const auto output = root / "output.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);

  struct rlimit previous{};
  ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &previous), 0);
  const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
  struct rlimit constrained = previous;
  constrained.rlim_cur = std::min<rlim_t>(previous.rlim_max, 4096U);
  ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &constrained), 0);
  const auto written = axk::write_hds_image(manifest_value, output);
  ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &previous), 0);
  std::signal(SIGXFSZ, previous_handler);

  ASSERT_FALSE(written);
  EXPECT_FALSE(std::filesystem::exists(output));
  EXPECT_TRUE(std::filesystem::is_empty(root));
  std::filesystem::remove_all(root, error);
}

TEST(HdsWriter, PermissionLossLeavesNoTemporaryOutput) {
  if (::geteuid() == 0)
    GTEST_SKIP() << "permission checks are not meaningful as root";

  axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {}};
  axk::VolumeSpec volume;
  volume.name = "Volume";
  manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
  const auto root = std::filesystem::temp_directory_path() /
                    ("axklib-writer-permission-" + std::to_string(::getpid()));
  const auto output = root / "output.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  std::filesystem::permissions(
      root, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);
  const auto written = axk::write_hds_image(manifest_value, output);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);

  ASSERT_FALSE(written);
  EXPECT_FALSE(std::filesystem::exists(output));
  EXPECT_TRUE(std::filesystem::is_empty(root));
  std::filesystem::remove_all(root, error);
}
#endif
