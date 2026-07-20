#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <tuple>

#if defined(__unix__)
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "../src/writer_internal.hpp"
#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

constexpr std::string_view manifest = R"json({
  "schema_version":"1.1",
  "size_bytes":536870912,
  "partitions":[{
    "name":"P1",
    "volumes":[{
      "name":"V1",
      "waveforms":[{"id":"tone","name":"Tone","path":"audio/tone.wav","root_key":60}],
      "samples":[{"name":"Tone Sample","waveform_id":"tone","root_key":60,"key_low":0,"key_high":127}]
    }]
  }]
})json";

axk::VolumeSpec graph_volume(const std::filesystem::path &audio_path) {
    axk::VolumeSpec volume;
    volume.name = "Graph Volume";
    volume.waveforms.push_back({"wave", "Graph Wave", audio_path, 60U, {}});

    axk::SampleSpec banked;
    banked.name = "Grouped Sample";
    banked.waveform_id = "wave";
    banked.root_key = 60U;
    banked.key_low = 0U;
    banked.key_high = 127U;
    volume.samples.push_back(std::move(banked));

    axk::SampleSpec direct;
    direct.name = "Direct Sample";
    direct.waveform_id = "wave";
    direct.root_key = 60U;
    direct.key_low = 0U;
    direct.key_high = 127U;
    volume.samples.push_back(std::move(direct));
    volume.sample_banks.push_back({"Graph Bank", {"Grouped Sample"}});
    volume.programs.push_back({1U, {{"SBAC", "Graph Bank", 1U}, {"SBNK", "Direct Sample", 2U}}});
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

TEST(HdsManifest, AcceptsPartitionsWithoutVolumes) {
    constexpr std::string_view empty = R"json({
      "schema_version":"1.1",
      "size_bytes":1048576,
      "partitions":[{"name":"P1","volumes":[]}]
    })json";
    const auto parsed = axk::parse_hds_build_manifest(empty);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed->partitions.size(), 1U);
    EXPECT_TRUE(parsed->partitions.front().volumes.empty());
}

TEST(HdsManifest, MigratesLegacySampleAndSampleBankFields) {
    constexpr std::string_view legacy = R"json({
      "schema_version":"1.0",
      "size_bytes":1048576,
      "partitions":[{"name":"P1","volumes":[{
        "name":"V1",
        "waveforms":[{"id":"wave","name":"Wave","path":"wave.wav","root_key":60}],
        "sample_banks":[{"name":"Sample","waveform_id":"wave","root_key":60,"key_low":0,"key_high":127}],
        "sample_bank_groups":[{"name":"Bank","member_sample_banks":["Sample"]}],
        "programs":[{"number":1,"assignments":[
          {"sample_bank_group":"Bank","receive_channel":1},
          {"sample_bank":"Sample","receive_channel":2}
        ]}]
      }]}]
    })json";
    const auto parsed = axk::parse_hds_build_manifest(legacy, "/project");
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->schema_version, "1.0");
    const auto &volume = parsed->partitions[0].volumes[0];
    ASSERT_EQ(volume.samples.size(), 1U);
    EXPECT_EQ(volume.samples[0].name, "Sample");
    ASSERT_EQ(volume.sample_banks.size(), 1U);
    EXPECT_EQ(volume.sample_banks[0].member_samples, std::vector<std::string>{"Sample"});
    ASSERT_EQ(volume.programs[0].assignments.size(), 2U);
    EXPECT_EQ(volume.programs[0].assignments[0].target_kind, "SBAC");
    EXPECT_EQ(volume.programs[0].assignments[1].target_kind, "SBNK");
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

TEST(BuildManifestTemplate, EmitsParseableHdsFloppyAndIsoStarters) {
    const auto hds = axk::serialize_build_manifest_template(axk::BuildManifestKind::hds);
    ASSERT_TRUE(hds) << hds.error().message;
    const auto parsed_hds = axk::parse_hds_build_manifest(*hds);
    ASSERT_TRUE(parsed_hds) << parsed_hds.error().message;
    ASSERT_EQ(parsed_hds->partitions.size(), 1U);
    EXPECT_TRUE(parsed_hds->partitions.front().volumes.empty());
    EXPECT_EQ(parsed_hds->size_bytes, 536'870'912U);

    const auto floppy = axk::serialize_build_manifest_template(axk::BuildManifestKind::fat12_floppy);
    ASSERT_TRUE(floppy) << floppy.error().message;
    const auto parsed_floppy = axk::parse_media_build_manifest(*floppy, "manifest-root");
    ASSERT_TRUE(parsed_floppy) << parsed_floppy.error().message;
    ASSERT_TRUE(parsed_floppy->authored_volume);
    EXPECT_EQ(parsed_floppy->format, axk::MediaImageFormat::fat12_floppy);
    ASSERT_EQ(parsed_floppy->authored_volume->waveforms.size(), 1U);
    EXPECT_EQ(parsed_floppy->authored_volume->waveforms.front().path.filename(), "tone.wav");

    const auto iso = axk::serialize_build_manifest_template(axk::BuildManifestKind::iso9660);
    ASSERT_TRUE(iso) << iso.error().message;
    const auto parsed_iso = axk::parse_media_build_manifest(*iso);
    ASSERT_TRUE(parsed_iso) << parsed_iso.error().message;
    ASSERT_TRUE(parsed_iso->authored_volume);
    EXPECT_EQ(parsed_iso->format, axk::MediaImageFormat::iso9660);
    EXPECT_TRUE(parsed_iso->authored_volume->waveforms.empty());
    EXPECT_TRUE(parsed_iso->authored_volume->samples.empty());

    EXPECT_FALSE(axk::serialize_build_manifest_template(
        static_cast<axk::BuildManifestKind>(std::numeric_limits<std::uint8_t>::max())));
}

TEST(BuildManifestTemplate, PublishesAtomicallyAndRequiresExplicitOverwrite) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-build-manifest-template-test";
    const auto path = root / "nested" / "image.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ASSERT_TRUE(axk::write_build_manifest_template(axk::BuildManifestKind::hds, path));
    ASSERT_TRUE(std::filesystem::is_regular_file(path));
    EXPECT_FALSE(axk::write_build_manifest_template(axk::BuildManifestKind::iso9660, path));
    ASSERT_TRUE(axk::write_build_manifest_template(axk::BuildManifestKind::iso9660, path, true));
    const auto parsed = axk::load_media_build_manifest(path);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->format, axk::MediaImageFormat::iso9660);

    std::filesystem::remove_all(root, error);
}

TEST(HdsGeometry, CoversEveryPartitionCountAtOneAndTwoGiBBoundaries) {
    for (std::uint8_t count = 1; count <= 8; ++count) {
        axk::HdsBuildManifest value{"1.1", count == 1 ? axk::minimum_hds_size : axk::maximum_hds_size, {}};
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

TEST(HdsCreationProfiles, PublishExactCapacitiesDefaultsAndAdmittedPartitionCounts) {
    const auto &profiles = axk::hds_creation_profiles();
    ASSERT_EQ(profiles.size(), 5U);
    EXPECT_EQ(axk::hds_creation_profile_id(profiles[0].id), "floppy-scale");
    EXPECT_EQ(profiles[0].size_bytes, 1'474'560U);
    EXPECT_EQ(profiles[0].default_partition_count, 1U);
    ASSERT_EQ(profiles[0].partition_options.size(), 1U);
    EXPECT_EQ(profiles[0].partition_options[0].partition_count, 1U);

    EXPECT_EQ(axk::hds_creation_profile_id(profiles[1].id), "cd-r-650");
    EXPECT_EQ(profiles[1].size_bytes, 681'984'000U);
    EXPECT_EQ(axk::hds_creation_profile_id(profiles[2].id), "cd-r-700");
    EXPECT_EQ(profiles[2].size_bytes, 737'280'000U);
    EXPECT_EQ(axk::hds_creation_profile_id(profiles[3].id), "hds-1-gib");
    EXPECT_EQ(profiles[3].size_bytes, 1'073'741'824U);
    for (std::size_t index = 1; index <= 3; ++index) {
        EXPECT_EQ(profiles[index].default_partition_count, 1U);
        ASSERT_EQ(profiles[index].partition_options.size(), 8U);
        for (std::size_t option = 0; option < 8U; ++option)
            EXPECT_EQ(profiles[index].partition_options[option].partition_count, option + 1U);
    }

    EXPECT_EQ(axk::hds_creation_profile_id(profiles[4].id), "hds-2-gib");
    EXPECT_EQ(profiles[4].size_bytes, 2'147'483'648U);
    EXPECT_EQ(profiles[4].default_partition_count, 2U);
    ASSERT_EQ(profiles[4].partition_options.size(), 7U);
    for (std::size_t option = 0; option < 7U; ++option)
        EXPECT_EQ(profiles[4].partition_options[option].partition_count, option + 2U);

    for (const auto &profile : profiles) {
        const auto parsed = axk::parse_hds_creation_profile_id(axk::hds_creation_profile_id(profile.id));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, profile.id);
    }
    EXPECT_FALSE(axk::parse_hds_creation_profile_id("unknown"));
}

TEST(HdsCreationProfiles, PlanCanonicalEmptyPartitionsThroughTheRegularWriterPlanner) {
    const auto planned = axk::plan_hds_creation({axk::HdsCreationProfileId::cd_r_700, 3U});
    ASSERT_TRUE(planned) << planned.error().message;
    EXPECT_EQ(planned->manifest.size_bytes, 737'280'000U);
    ASSERT_EQ(planned->manifest.partitions.size(), 3U);
    ASSERT_EQ(planned->summary.partitions.size(), 3U);
    EXPECT_EQ(planned->summary.object_count, 0U);
    EXPECT_EQ(planned->summary.size_bytes, planned->manifest.size_bytes);
    for (std::size_t index = 0; index < planned->manifest.partitions.size(); ++index) {
        const auto &partition = planned->manifest.partitions[index];
        EXPECT_EQ(partition.name, "PARTITION " + std::to_string(index + 1U));
        EXPECT_TRUE(partition.volumes.empty());
    }

    EXPECT_FALSE(axk::plan_hds_creation({axk::HdsCreationProfileId::floppy_scale, 2U}));
    EXPECT_FALSE(axk::plan_hds_creation({axk::HdsCreationProfileId::hds_2_gib, 1U}));
    EXPECT_TRUE(axk::plan_hds_creation({axk::HdsCreationProfileId::hds_2_gib, 2U}));
    EXPECT_FALSE(axk::plan_hds_creation({static_cast<axk::HdsCreationProfileId>(255U), 1U}));
}

TEST(AudioImport, PreservesNativePcm16AndChoosesOnlyHardwareRates) {
    axk::Waveform source;
    source.format = {1, 2, 48000};
    source.frame_count = 3;
    source.pcm = {std::byte{0x00}, std::byte{0x80}, std::byte{0x34}, std::byte{0x12}, std::byte{0xff}, std::byte{0x7f}};
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

TEST(AudioImport, SmplSerializationRejectsWaveDataPastTheHardwareAddressLimit) {
    axk::ImportedAudio audio;
    audio.output_sample_rate = 44'100U;
    audio.output_frames = axk::maximum_wave_data_frames_per_channel + 1U;
    audio.pcm_channels = {{std::byte{}, std::byte{}}};
    axk::WaveformSpec waveform;
    waveform.name = "Too Large";

    const auto payload = axk::detail::prepare_smpl_payload(waveform, audio, 0x100U);
    ASSERT_FALSE(payload);
    EXPECT_EQ(payload.error().code, axk::ErrorCode::audio_wave_data_too_large);

    audio.output_frames = 1U;
    audio.pcm_channels = {
        std::vector<std::byte>(static_cast<std::size_t>(axk::maximum_wave_data_pcm16_bytes_per_channel + 2U))};
    const auto oversized_pcm = axk::detail::prepare_smpl_payload(waveform, audio, 0x100U);
    ASSERT_FALSE(oversized_pcm);
    EXPECT_EQ(oversized_pcm.error().code, axk::ErrorCode::audio_wave_data_too_large);
}

TEST(HdsWriter, AtomicallyWritesAndReopensFreshEmptyVolumeImage) {
    axk::HdsBuildManifest manifest_value{"1.1", axk::minimum_hds_size, {}};
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

TEST(HdsWriter, AtomicallyWritesAndReopensPartitionWithoutVolumes) {
    axk::HdsBuildManifest manifest_value{"1.1", axk::minimum_hds_size, {{"hd1", {}}}};
    const auto path = std::filesystem::temp_directory_path() / "axklib-native-empty-partition.hds";
    std::error_code error;
    std::filesystem::remove(path, error);
    const auto written = axk::write_hds_image(manifest_value, path);
    ASSERT_TRUE(written) << written.error().message;
    const auto reopened = axk::open_image(path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened->partitions().size(), 1U);
    const auto &root = *std::ranges::find(reopened->partitions()[0].records, axk::SfsId{1}, &axk::IndexRecord::sfs_id);
    EXPECT_EQ(std::ranges::count_if(root.directory_entries,
                                    [](const auto &entry) {
                                        return entry.name != "." && entry.name != ".." && entry.name != "sfserrlog" &&
                                               entry.name != "sfserram";
                                    }),
              0U);
    std::filesystem::remove(path, error);
}

TEST(HdsWriter, WritesMonoSampleAndRoundTripsExactPhysicalPcm) {
    axk::Waveform source;
    source.format = {1, 2, 44100};
    source.frame_count = 3;
    source.pcm = {std::byte{}, std::byte{}, std::byte{0xe8}, std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
    const auto audio_path = std::filesystem::temp_directory_path() / "axklib-writer-sample.wav";
    const auto image_path = std::filesystem::temp_directory_path() / "axklib-writer-sample.hds";
    std::error_code error;
    std::filesystem::remove(audio_path, error);
    std::filesystem::remove(image_path, error);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));

    axk::WaveformSpec waveform{"wave", "Wave", audio_path, 60, {}};
    axk::SampleSpec sample;
    sample.name = "Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60;
    sample.key_low = 48;
    sample.key_high = 72;
    sample.level = 96;
    axk::VolumeSpec volume;
    volume.name = "Volume";
    volume.waveforms.push_back(std::move(waveform));
    volume.samples.push_back(std::move(sample));
    axk::HdsBuildManifest manifest_value{"1.1", 4U * 1024U * 1024U, {}};
    manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
    ASSERT_TRUE(axk::write_hds_image(manifest_value, image_path));
    const auto reopened = axk::open_image(image_path);
    ASSERT_TRUE(reopened);
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog);
    const auto type = [](const axk::ObjectSnapshot &item) { return item.object.header.type; };
    EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::smpl, type), 1U);
    EXPECT_EQ(std::ranges::count(catalog->objects, axk::ObjectType::sbnk, type), 1U);
    const auto wave_data = std::ranges::find(catalog->objects, axk::ObjectType::smpl, type);
    ASSERT_NE(wave_data, catalog->objects.end());
    const auto decoded = axk::decode_waveform(*reopened, *wave_data);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::ranges::equal(source.pcm, std::span<const std::byte>{decoded->pcm}.first(source.pcm.size())));
    std::filesystem::remove(audio_path, error);
    std::filesystem::remove(image_path, error);
}

TEST(HdsWriter, CancellationPublishesNoImageOrTemporarySibling) {
    axk::HdsBuildManifest manifest_value{"1.1", axk::minimum_hds_size, {}};
    axk::VolumeSpec volume;
    volume.name = "Volume";
    manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
    const auto path = std::filesystem::temp_directory_path() / "axklib-cancelled-writer.hds";
    std::error_code error;
    std::filesystem::remove(path, error);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto written = axk::write_hds_image(manifest_value, path, false, cancellation.token());
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(path));
    for (const auto &entry : std::filesystem::directory_iterator{path.parent_path()}) {
        const auto name = entry.path().filename().string();
        EXPECT_FALSE(name.starts_with("." + path.filename().string() + ".axklib-publication.p"));
    }
}

TEST(HdsWriter, OverwriteReplacesSymlinkWithoutFollowingItsTarget) {
    axk::HdsBuildManifest manifest_value{"1.1", axk::minimum_hds_size, {}};
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

TEST(ImageBuildPlanning, PreparesEveryInputWithoutPublishingAnOutput) {
    axk::HdsBuildManifest hds{"1.1", axk::minimum_hds_size, {}};
    axk::VolumeSpec volume;
    volume.name = "Volume";
    volume.waveforms.push_back({"missing", "Missing", "missing.wav", 60U, {}});
    hds.partitions.push_back({"hd1", {volume}});

    const auto invalid_hds = axk::plan_hds_build(hds);
    ASSERT_FALSE(invalid_hds);

    hds.partitions.front().volumes.front().waveforms.clear();
    const auto valid_hds = axk::plan_hds_build(hds);
    ASSERT_TRUE(valid_hds) << valid_hds.error().message;
    EXPECT_EQ(valid_hds->size_bytes, axk::minimum_hds_size);
    EXPECT_EQ(valid_hds->partition_count, 1U);
    EXPECT_EQ(valid_hds->object_count, 0U);

    axk::MediaBuildManifest iso;
    iso.schema_version = "1.1";
    iso.format = axk::MediaImageFormat::iso9660;
    iso.authored_volume = volume;
    const auto invalid_iso = axk::plan_media_build(iso);
    ASSERT_FALSE(invalid_iso);

    iso.authored_volume->waveforms.clear();
    const auto valid_iso = axk::plan_media_build(iso);
    ASSERT_TRUE(valid_iso) << valid_iso.error().message;
    EXPECT_EQ(valid_iso->format, axk::MediaImageFormat::iso9660);
    EXPECT_EQ(valid_iso->object_count, 0U);
}

TEST(MediaManifest, ParsesStrictAuthoredAndTransferModes) {
    constexpr std::string_view authored = R"json({
    "schema_version":"1.1",
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
      "samples":[{
        "name":"Tone Sample","waveform_id":"tone","root_key":60,"key_low":0,"key_high":127
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
    "schema_version":"1.1",
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
    invalid.insert(invalid.find("\"authored_volume\""), "\"transfer\":{\"source_path\":\"source.hds\",\"root_object_"
                                                        "keys\":[\"x\"]},");
    EXPECT_FALSE(axk::parse_media_build_manifest(invalid));
}

TEST(MediaWriter, WritesDeterministicFat12AndIso9660ImagesAndReopensExactPcm) {
    axk::Waveform source;
    source.format = {1, 2, 44100};
    source.frame_count = 3;
    source.pcm = {std::byte{}, std::byte{}, std::byte{0xe8}, std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-writer";
    const auto audio_path = root / "tone.wav";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));

    axk::WaveformSpec waveform{"wave", "Wave", audio_path, 60, {}};
    axk::SampleSpec sample;
    sample.name = "Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60;
    sample.key_high = 127;
    axk::VolumeSpec volume;
    volume.name = "Volume";
    volume.waveforms.push_back(std::move(waveform));
    volume.samples.push_back(std::move(sample));

    for (const auto format : {axk::MediaImageFormat::fat12_floppy, axk::MediaImageFormat::iso9660}) {
        axk::MediaBuildManifest manifest_value;
        manifest_value.schema_version = "1.1";
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
            EXPECT_EQ(*group_label_bytes,
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
                            std::tuple{"GROUP/F001/SBNK/0000", "Sample", std::byte{0xc9}}}) {
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
            const auto wave_data_catalog = find_file("GROUP/F001/SMPL/0000");
            ASSERT_NE(wave_data_catalog, iso->files().end());
            const auto catalog_offset = static_cast<std::size_t>(wave_data_catalog->extent_sector) * 2048U;

            const auto wave_data_object = find_file("GROUP/F001/SMPL/F001");
            ASSERT_NE(wave_data_object, iso->files().end());
            auto missing_tail_bytes = first_bytes;
            missing_tail_bytes.resize(static_cast<std::size_t>(wave_data_object->extent_sector) * 2048U);
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
            std::ranges::copy(shifted_record, shifted_bytes.begin() + static_cast<std::ptrdiff_t>(catalog_offset));
            std::ranges::fill_n(shifted_bytes.begin() + static_cast<std::ptrdiff_t>(catalog_offset + 28U), 4U, '\0');
            const auto shifted_path = root / "shifted-category.iso";
            std::ofstream shifted_output{shifted_path, std::ios::binary | std::ios::trunc};
            shifted_output.write(shifted_bytes.data(), static_cast<std::streamsize>(shifted_bytes.size()));
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
            damaged_output.write(damaged_bytes.data(), static_cast<std::streamsize>(damaged_bytes.size()));
            damaged_output.close();
            const auto damaged = axk::IsoImage::open(damaged_path);
            ASSERT_TRUE(damaged) << damaged.error().message;
            EXPECT_NE(std::ranges::find(damaged->validation_issues(), std::string{"ISO_YAMAHA_CATEGORY_OBJECT_MISSING"},
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
        const auto wave_data = std::ranges::find(objects->begin(), objects->end(), axk::ObjectType::smpl,
                                                 [](const auto &object) { return object.decoded.header.type; });
        ASSERT_NE(wave_data, objects->end());
        const auto decoded = axk::decode_waveform(*wave_data);
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_TRUE(std::ranges::equal(source.pcm, std::span<const std::byte>{decoded->pcm}.first(source.pcm.size())));
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
    value.schema_version = "1.1";
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

TEST(MediaWriter, AllowsObjectEmptyIsoPackageStagingTargetOnly) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-empty-media-staging";
    const auto iso_path = root / "empty.iso";
    const auto floppy_path = root / "empty.ima";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::VolumeSpec volume;
    volume.name = "Import Target";
    axk::MediaBuildManifest media_manifest;
    media_manifest.schema_version = "1.1";
    media_manifest.format = axk::MediaImageFormat::iso9660;
    media_manifest.authored_volume = volume;
    media_manifest.iso_volume_id = "AXK_STAGING";
    media_manifest.raw_group = "46DEF120";
    media_manifest.group_name = "Package Import";
    media_manifest.raw_volume = "F001";
    media_manifest.volume_name = volume.name;

    const auto written = axk::write_media_image(media_manifest, iso_path);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written->object_count, 0U);
    const auto media = axk::open_media(iso_path);
    ASSERT_TRUE(media) << media.error().message;
    const auto objects = media->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    EXPECT_TRUE(objects->empty());

    media_manifest.format = axk::MediaImageFormat::fat12_floppy;
    const auto floppy = axk::write_media_image(media_manifest, floppy_path);
    ASSERT_FALSE(floppy);
    EXPECT_NE(floppy.error().message.find("at least one Yamaha object"), std::string::npos);
    std::filesystem::remove_all(root, error);
}

TEST(MediaWriter, SavedObjectTransferAddsKnownSampleDependencies) {
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
    axk::HdsBuildManifest hds{"1.1", 4U * 1024U * 1024U, {{"hd1", {volume}}}};
    ASSERT_TRUE(axk::write_hds_image(hds, source_path));
    const auto source_media = axk::open_media(source_path);
    ASSERT_TRUE(source_media);
    const auto source_objects = source_media->objects();
    ASSERT_TRUE(source_objects);
    const auto sample_object = std::ranges::find(*source_objects, axk::ObjectType::sbnk,
                                                 [](const auto &object) { return object.decoded.header.type; });
    ASSERT_NE(sample_object, source_objects->end());

    axk::MediaBuildManifest transfer;
    transfer.schema_version = "1.1";
    transfer.format = axk::MediaImageFormat::fat12_floppy;
    transfer.transfer = axk::SavedObjectTransferSpec{source_path, {sample_object->key}};
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

    const auto program_object = std::ranges::find(*source_objects, axk::ObjectType::prog,
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
    whole.schema_version = "1.1";
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
    axk::HdsBuildManifest manifest_value{"1.1", axk::minimum_hds_size, {}};
    axk::VolumeSpec volume;
    volume.name = "Volume";
    manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
    const auto root =
        std::filesystem::temp_directory_path() / ("axklib-writer-disk-full-" + std::to_string(::getpid()));
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

    axk::HdsBuildManifest manifest_value{"1.1", axk::minimum_hds_size, {}};
    axk::VolumeSpec volume;
    volume.name = "Volume";
    manifest_value.partitions.push_back({"hd1", {std::move(volume)}});
    const auto root =
        std::filesystem::temp_directory_path() / ("axklib-writer-permission-" + std::to_string(::getpid()));
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    const auto written = axk::write_hds_image(manifest_value, output);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

    ASSERT_FALSE(written);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_TRUE(std::filesystem::is_empty(root));
    std::filesystem::remove_all(root, error);
}
#endif
