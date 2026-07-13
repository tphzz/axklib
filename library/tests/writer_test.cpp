#include <algorithm>
#include <array>
#include <fstream>
#include <tuple>

#if defined(__unix__)
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
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

axk::VolumeSpec graph_volume(const std::filesystem::path &audio_path) {
  axk::VolumeSpec volume;
  volume.name = "Graph Volume";
  volume.waveforms.push_back({"wave", "Graph Wave", audio_path, 60U, {}});

  axk::SampleBankSpec grouped;
  grouped.name = "Grouped Bank";
  grouped.waveform_id = "wave";
  grouped.root_key = 60U;
  grouped.key_low = 0U;
  grouped.key_high = 127U;
  volume.sample_banks.push_back(std::move(grouped));

  axk::SampleBankSpec direct;
  direct.name = "Direct Bank";
  direct.waveform_id = "wave";
  direct.root_key = 60U;
  direct.key_low = 0U;
  direct.key_high = 127U;
  volume.sample_banks.push_back(std::move(direct));
  volume.sample_bank_groups.push_back({"Graph Group", {"Grouped Bank"}});
  volume.programs.push_back({1U, {{"SBAC", "Graph Group", 1U}, {"SBNK", "Direct Bank", 2U}}});
  return volume;
}

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

TEST(MediaManifest, ParsesStrictAuthoredAndTransferModes) {
  constexpr std::string_view authored = R"json({
    "schema_version":"1.0",
    "format":"iso9660",
    "iso":{
      "volume_id":"AXK_TEST",
      "raw_group":"GROUP",
      "group_name":"Test Group",
      "raw_volume":"F001",
      "volume_name":"Test Volume"
    },
    "authored_volume":{
      "name":"Test Volume",
      "waveforms":[{"id":"tone","name":"Tone","path":"tone.wav","root_key":60}],
      "sample_banks":[{
        "name":"Tone Bank","waveform_id":"tone","root_key":60,"key_low":0,"key_high":127
      }]
    }
  })json";
  const auto parsed = axk::parse_media_build_manifest(authored, "/project");
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed->format, axk::MediaImageFormat::iso9660);
  ASSERT_TRUE(parsed->authored_volume);
  EXPECT_EQ(parsed->authored_volume->waveforms.front().path, "/project/tone.wav");
  EXPECT_EQ(parsed->volume_name, "Test Volume");

  constexpr std::string_view whole_source = R"json({
    "schema_version":"1.0",
    "format":"iso9660",
    "iso":{
      "volume_id":"AXK_TEST",
      "raw_group":"00000010",
      "group_name":"Test Group",
      "raw_volume":"F001",
      "volume_name":"Test Volume"
    },
    "transfer":{"source_path":"source.ima","selection":"all"}
  })json";
  const auto whole = axk::parse_media_build_manifest(whole_source, "/project");
  ASSERT_TRUE(whole) << whole.error().message;
  ASSERT_TRUE(whole->transfer);
  EXPECT_EQ(whole->transfer->source_path, "/project/source.ima");
  EXPECT_EQ(whole->transfer->selection, axk::SavedObjectSelection::all);
  EXPECT_TRUE(whole->transfer->root_object_keys.empty());

  auto conflicting_selection = std::string{whole_source};
  conflicting_selection.replace(conflicting_selection.find("\"selection\":\"all\""), 17,
                                "\"selection\":\"all\",\"root_object_keys\":[\"x\"]");
  EXPECT_FALSE(axk::parse_media_build_manifest(conflicting_selection));

  auto invalid = std::string{authored};
  invalid.insert(invalid.find("\"authored_volume\""),
                 "\"transfer\":{\"source_path\":\"source.hds\",\"root_object_keys\":[\"x\"]},");
  EXPECT_FALSE(axk::parse_media_build_manifest(invalid));
}

TEST(MediaWriter, WritesDeterministicFat12AndIso9660ImagesAndReopensExactPcm) {
  axk::Waveform source;
  source.format = {1, 2, 44100};
  source.frame_count = 3;
  source.pcm = {std::byte{},     std::byte{},     std::byte{0xe8},
                std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
  const auto root = std::filesystem::temp_directory_path() / "axklib-media-writer";
  const auto audio_path = root / "tone.wav";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));

  axk::WaveformSpec waveform{"wave", "Wave", audio_path, 60, {}};
  axk::SampleBankSpec bank;
  bank.name = "Bank";
  bank.waveform_id = "wave";
  bank.root_key = 60;
  bank.key_high = 127;
  axk::VolumeSpec volume;
  volume.name = "Volume";
  volume.waveforms.push_back(std::move(waveform));
  volume.sample_banks.push_back(std::move(bank));

  for (const auto format : {axk::MediaImageFormat::fat12_floppy, axk::MediaImageFormat::iso9660}) {
    axk::MediaBuildManifest manifest_value;
    manifest_value.schema_version = "1.0";
    manifest_value.format = format;
    manifest_value.authored_volume = volume;
    manifest_value.iso_volume_id = "AXK_TEST";
    manifest_value.raw_group = "GROUP";
    manifest_value.group_name = "Test Group";
    manifest_value.raw_volume = "F001";
    manifest_value.volume_name = "Test Volume";
    const auto extension = format == axk::MediaImageFormat::fat12_floppy ? ".ima" : ".iso";
    const auto first = root / ("first" + std::string{extension});
    const auto second = root / ("second" + std::string{extension});
    const auto written = axk::write_media_image(manifest_value, first);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written->object_count, 2U);
    EXPECT_FALSE(axk::write_media_image(manifest_value, first));
    ASSERT_TRUE(axk::write_media_image(manifest_value, second));

    std::ifstream first_input{first, std::ios::binary};
    std::ifstream second_input{second, std::ios::binary};
    const std::vector<char> first_bytes{std::istreambuf_iterator<char>{first_input}, {}};
    const std::vector<char> second_bytes{std::istreambuf_iterator<char>{second_input}, {}};
    EXPECT_EQ(first_bytes, second_bytes);

    if (format == axk::MediaImageFormat::fat12_floppy) {
      ASSERT_EQ(first_bytes.size(), 1'474'560U);
      EXPECT_EQ((std::string_view{first_bytes.data(), 3U}), (std::string_view{"\xeb\x58\x90", 3U}));
      EXPECT_EQ((std::string_view{first_bytes.data() + 3U, 8U}), "WINIMAGE");
      EXPECT_EQ(static_cast<unsigned char>(first_bytes[21]), 0xf0U);
      EXPECT_EQ(static_cast<unsigned char>(first_bytes[24]), 18U);
      EXPECT_EQ(static_cast<unsigned char>(first_bytes[26]), 2U);
      EXPECT_EQ(static_cast<unsigned char>(first_bytes[36]), 0U);
      EXPECT_EQ((std::string_view{first_bytes.data() + 43U, 11U}), "           ");
      EXPECT_EQ((std::string_view{first_bytes.data() + 54U, 8U}), "FAT12   ");
      EXPECT_EQ(static_cast<unsigned char>(first_bytes[512]), 0xf0U);
      EXPECT_EQ(static_cast<unsigned char>(first_bytes[512U + 9U * 512U]), 0xf0U);
    } else {
      ASSERT_GE(first_bytes.size(), (16U * 2048U) + 40U);
      const std::string_view system_id{first_bytes.data() + (16U * 2048U) + 8U, 32U};
      EXPECT_EQ(system_id, "APPLE COMPUTER, INC., TYPE: 0002");

      const auto iso = axk::IsoImage::open(first);
      ASSERT_TRUE(iso) << iso.error().message;
      EXPECT_TRUE(iso->validation_issues().empty());
      const auto find_file = [&](std::string_view path) {
        return std::ranges::find(iso->files(), path, &axk::IsoFile::path);
      };
      const auto expected_catalog = [](std::string_view name, std::byte name_hash) {
        std::vector<std::byte> result(32U);
        std::fill_n(result.begin() + 1, 16U, std::byte{' '});
        std::ranges::transform(name, result.begin() + 1,
                               [](char character) { return static_cast<std::byte>(character); });
        result[0] = name_hash;
        result[17] = std::byte{0x5d};
        result[18] = std::byte{'F'};
        result[19] = std::byte{'0'};
        result[20] = std::byte{'0'};
        result[21] = std::byte{'1'};
        return result;
      };
      const auto group_label = find_file("GROUP/F002");
      ASSERT_NE(group_label, iso->files().end());
      const auto group_label_bytes = iso->read_file(*group_label);
      ASSERT_TRUE(group_label_bytes);
      EXPECT_EQ(
          *group_label_bytes,
          (std::vector<std::byte>{std::byte{'T'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'},
                                  std::byte{' '}, std::byte{'G'}, std::byte{'r'}, std::byte{'o'},
                                  std::byte{'u'}, std::byte{'p'}, std::byte{' '}, std::byte{' '},
                                  std::byte{' '}, std::byte{' '}, std::byte{' '}, std::byte{' '}}));

      const auto volume_catalog = find_file("GROUP/0000");
      ASSERT_NE(volume_catalog, iso->files().end());
      const auto volume_catalog_bytes = iso->read_file(*volume_catalog);
      ASSERT_TRUE(volume_catalog_bytes);
      auto expected_group_catalog = expected_catalog("Test Volume", std::byte{0xd8});
      std::vector<std::byte> expected_disk_name(32U);
      expected_disk_name[0] = std::byte{0xe1};
      std::ranges::transform(std::string_view{"_DSKNAME"}, expected_disk_name.begin() + 1,
                             [](char character) { return static_cast<std::byte>(character); });
      expected_disk_name[17] = std::byte{0x5e};
      std::ranges::transform(std::string_view{"F002"}, expected_disk_name.begin() + 18,
                             [](char character) { return static_cast<std::byte>(character); });
      expected_group_catalog.insert(expected_group_catalog.end(), expected_disk_name.begin(),
                                    expected_disk_name.end());
      EXPECT_EQ(*volume_catalog_bytes, expected_group_catalog);

      for (const auto &[path, name, expected_hash] :
           std::array{std::tuple{"GROUP/F001/SMPL/0000", "Wave", std::byte{0xfa}},
                      std::tuple{"GROUP/F001/SBNK/0000", "Bank", std::byte{0xc7}}}) {
        const auto catalog_file = find_file(path);
        ASSERT_NE(catalog_file, iso->files().end());
        const auto catalog_bytes = iso->read_file(*catalog_file);
        ASSERT_TRUE(catalog_bytes);
        EXPECT_EQ(*catalog_bytes, expected_catalog(name, expected_hash));
      }
      EXPECT_NE(find_file("GROUP/F001/SMPL/F001"), iso->files().end());
      EXPECT_NE(find_file("GROUP/F001/SBNK/F001"), iso->files().end());
      EXPECT_EQ(find_file("GROUP/F001/SMPL/F000"), iso->files().end());

      auto damaged_bytes = first_bytes;
      const auto sample_catalog = find_file("GROUP/F001/SMPL/0000");
      ASSERT_NE(sample_catalog, iso->files().end());
      const auto catalog_offset = static_cast<std::size_t>(sample_catalog->extent_sector) * 2048U;

      const auto sample_object = find_file("GROUP/F001/SMPL/F001");
      ASSERT_NE(sample_object, iso->files().end());
      auto missing_tail_bytes = first_bytes;
      missing_tail_bytes.resize(static_cast<std::size_t>(sample_object->extent_sector) * 2048U);
      const auto missing_tail_path = root / "missing-object-tail.iso";
      std::ofstream missing_tail_output{missing_tail_path, std::ios::binary | std::ios::trunc};
      missing_tail_output.write(missing_tail_bytes.data(),
                                static_cast<std::streamsize>(missing_tail_bytes.size()));
      missing_tail_output.close();
      const auto missing_tail = axk::IsoImage::open(missing_tail_path);
      ASSERT_TRUE(missing_tail) << missing_tail.error().message;
      EXPECT_TRUE(missing_tail->validation_issues().empty());

      auto shifted_bytes = first_bytes;
      std::array<char, 28U> shifted_record{};
      std::ranges::copy_n(shifted_bytes.begin() + static_cast<std::ptrdiff_t>(catalog_offset + 4U),
                          shifted_record.size(), shifted_record.begin());
      std::ranges::copy(shifted_record,
                        shifted_bytes.begin() + static_cast<std::ptrdiff_t>(catalog_offset));
      std::ranges::fill_n(shifted_bytes.begin() + static_cast<std::ptrdiff_t>(catalog_offset + 28U),
                          4U, '\0');
      const auto shifted_path = root / "shifted-category.iso";
      std::ofstream shifted_output{shifted_path, std::ios::binary | std::ios::trunc};
      shifted_output.write(shifted_bytes.data(),
                           static_cast<std::streamsize>(shifted_bytes.size()));
      shifted_output.close();
      const auto shifted = axk::IsoImage::open(shifted_path);
      ASSERT_TRUE(shifted) << shifted.error().message;
      EXPECT_TRUE(shifted->validation_issues().empty());
      const auto shifted_objects = shifted->objects();
      ASSERT_TRUE(shifted_objects) << shifted_objects.error().message;
      EXPECT_EQ(shifted_objects->size(), 2U);

      std::ranges::copy(std::string_view{"F099"}, damaged_bytes.data() + catalog_offset + 18U);
      const auto damaged_path = root / "damaged-category.iso";
      std::ofstream damaged_output{damaged_path, std::ios::binary | std::ios::trunc};
      damaged_output.write(damaged_bytes.data(),
                           static_cast<std::streamsize>(damaged_bytes.size()));
      damaged_output.close();
      const auto damaged = axk::IsoImage::open(damaged_path);
      ASSERT_TRUE(damaged) << damaged.error().message;
      EXPECT_NE(std::ranges::find(damaged->validation_issues(),
                                  std::string{"ISO_YAMAHA_CATEGORY_OBJECT_MISSING"},
                                  &axk::MediaValidationIssue::code),
                damaged->validation_issues().end());
      const auto damaged_objects = damaged->objects();
      ASSERT_TRUE(damaged_objects) << damaged_objects.error().message;
      EXPECT_EQ(damaged_objects->size(), 2U);
    }

    const auto media = axk::open_media(first);
    ASSERT_TRUE(media) << media.error().message;
    const auto objects = media->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    const auto sample =
        std::ranges::find(objects->begin(), objects->end(), axk::ObjectType::smpl,
                          [](const auto &object) { return object.decoded.header.type; });
    ASSERT_NE(sample, objects->end());
    const auto decoded = axk::decode_waveform(*sample);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_TRUE(std::ranges::equal(
        source.pcm, std::span<const std::byte>{decoded->pcm}.first(source.pcm.size())));
  }
  std::filesystem::remove_all(root, error);
}

TEST(MediaWriter, AuthoredIsoReopensCompleteProgramHierarchy) {
  axk::Waveform source;
  source.format = {1, 2, 44100};
  source.frame_count = 4U;
  source.pcm = {std::byte{},     std::byte{},     std::byte{0x34}, std::byte{0x12},
                std::byte{0xcc}, std::byte{0xed}, std::byte{},     std::byte{}};
  const auto root = std::filesystem::temp_directory_path() / "axklib-media-graph";
  const auto audio_path = root / "graph.wav";
  const auto image_path = root / "graph.iso";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));

  axk::MediaBuildManifest value;
  value.schema_version = "1.0";
  value.format = axk::MediaImageFormat::iso9660;
  value.authored_volume = graph_volume(audio_path);
  value.iso_volume_id = "AXK_GRAPH";
  value.raw_group = "00000010";
  value.group_name = "Authored Graph";
  value.raw_volume = "F001";
  value.volume_name = "Graph Volume";
  const auto written = axk::write_media_image(value, image_path);
  ASSERT_TRUE(written) << written.error().message;
  EXPECT_EQ(written->object_count, 5U);

  const auto media = axk::open_media(image_path);
  ASSERT_TRUE(media) << media.error().message;
  EXPECT_TRUE(media->validation_issues().empty());
  const auto catalog = axk::build_object_catalog(*media);
  ASSERT_TRUE(catalog) << catalog.error().message;
  const auto type = [](const axk::ObjectSnapshot &item) { return item.object.header.type; };
  EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::smpl, type), 1U);
  EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::sbnk, type), 2U);
  EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::sbac, type), 1U);
  EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::prog, type), 1U);
  const auto graph = axk::build_relationship_graph(*catalog);
  const auto relationship_count = [&](std::string_view relationship_type) {
    return std::ranges::count(graph.relationships, relationship_type, &axk::Relationship::type);
  };
  EXPECT_EQ(relationship_count("SBNK_LEFT_MEMBER_TO_SMPL"), 2U);
  EXPECT_EQ(relationship_count("SBAC_SLOT_TO_SBNK"), 1U);
  EXPECT_EQ(relationship_count("PROG_ASSIGNMENT_TO_SBAC"), 1U);
  EXPECT_EQ(relationship_count("PROG_ASSIGNMENT_TO_SBNK"), 1U);
  std::filesystem::remove_all(root, error);
}

TEST(MediaWriter, SavedObjectTransferAddsKnownSampleBankDependencies) {
  axk::Waveform source;
  source.format = {1, 2, 44100};
  source.frame_count = 1;
  source.pcm = {std::byte{0x34}, std::byte{0x12}};
  const auto root = std::filesystem::temp_directory_path() / "axklib-media-transfer";
  const auto audio_path = root / "tone.wav";
  const auto source_path = root / "source.hds";
  const auto output_path = root / "transfer.ima";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));

  auto volume = graph_volume(audio_path);
  axk::HdsBuildManifest hds{"1.0", 4U * 1024U * 1024U, {{"hd1", {volume}}}};
  ASSERT_TRUE(axk::write_hds_image(hds, source_path));
  const auto source_media = axk::open_media(source_path);
  ASSERT_TRUE(source_media);
  const auto source_objects = source_media->objects();
  ASSERT_TRUE(source_objects);
  const auto bank_object =
      std::ranges::find(*source_objects, axk::ObjectType::sbnk,
                        [](const auto &object) { return object.decoded.header.type; });
  ASSERT_NE(bank_object, source_objects->end());

  axk::MediaBuildManifest transfer;
  transfer.schema_version = "1.0";
  transfer.format = axk::MediaImageFormat::fat12_floppy;
  transfer.transfer = axk::SavedObjectTransferSpec{source_path, {bank_object->key}};
  const auto written = axk::write_media_image(transfer, output_path);
  ASSERT_TRUE(written) << written.error().message;
  EXPECT_EQ(written->object_count, 2U);
  const auto output = axk::open_media(output_path);
  ASSERT_TRUE(output);
  const auto output_objects = output->objects();
  ASSERT_TRUE(output_objects);
  EXPECT_EQ(output_objects->size(), 2U);
  EXPECT_EQ(std::ranges::count(*output_objects, axk::ObjectType::smpl,
                               [](const auto &object) { return object.decoded.header.type; }),
            1U);

  const auto program_object =
      std::ranges::find(*source_objects, axk::ObjectType::prog,
                        [](const auto &object) { return object.decoded.header.type; });
  ASSERT_NE(program_object, source_objects->end());
  const auto program_path = root / "program.ima";
  transfer.transfer = axk::SavedObjectTransferSpec{source_path, {program_object->key}};
  const auto program_written = axk::write_media_image(transfer, program_path);
  ASSERT_TRUE(program_written) << program_written.error().message;
  EXPECT_EQ(program_written->object_count, 5U);
  const auto program_media = axk::open_media(program_path);
  ASSERT_TRUE(program_media);
  const auto program_objects = program_media->objects();
  ASSERT_TRUE(program_objects);
  EXPECT_EQ(program_objects->size(), 5U);

  axk::MediaBuildManifest whole;
  whole.schema_version = "1.0";
  whole.format = axk::MediaImageFormat::iso9660;
  whole.transfer = axk::SavedObjectTransferSpec{program_path, {}, axk::SavedObjectSelection::all};
  whole.iso_volume_id = "AXK_TRANSFER";
  whole.raw_group = "00000010";
  whole.group_name = "Transfer";
  whole.raw_volume = "F001";
  whole.volume_name = "Transfer";
  const auto whole_path = root / "whole.iso";
  const auto whole_written = axk::write_media_image(whole, whole_path);
  ASSERT_TRUE(whole_written) << whole_written.error().message;
  EXPECT_EQ(whole_written->object_count, program_objects->size());
  const auto whole_media = axk::open_media(whole_path);
  ASSERT_TRUE(whole_media);
  const auto whole_objects = whole_media->objects();
  ASSERT_TRUE(whole_objects);
  const auto sorted_payloads = [](const auto &objects) {
    std::vector<std::vector<std::byte>> payloads;
    payloads.reserve(objects.size());
    for (const auto &object : objects)
      payloads.push_back(object.raw_payload);
    std::ranges::sort(payloads, [](const auto &left, const auto &right) {
      return std::lexicographical_compare(
          left.begin(), left.end(), right.begin(), right.end(), [](std::byte lhs, std::byte rhs) {
            return std::to_integer<unsigned int>(lhs) < std::to_integer<unsigned int>(rhs);
          });
    });
    return payloads;
  };
  EXPECT_EQ(sorted_payloads(*program_objects), sorted_payloads(*whole_objects));

  whole.transfer->source_path = source_path;
  EXPECT_FALSE(axk::write_media_image(whole, root / "whole-from-hds.iso"));
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
