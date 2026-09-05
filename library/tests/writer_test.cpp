#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <vector>

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
#include "axklib/writer_internal.hpp"

namespace {

constexpr std::string_view manifest = R"json({
  "schema_version":"1.0",
  "size_bytes":536870912,
  "partitions":[{
    "name":"P1",
    "volumes":[{
      "name":"V1",
      "waveforms":[{"id":"tone","name":"Tone","path":"audio/tone.wav","root_key":60,
                    "fine_tune_cents":-17,"loop_mode":1,"loop_start_frame":4,"loop_length_frames":80}],
      "samples":[{"name":"Tone Sample","waveform_id":"tone","parameters":{
                  "root_key":60,"key_low":12,"key_high":96,"fine_tune_cents":-17,
                  "velocity_low":8,"velocity_high":110,"loop_mode":1,
                  "loop_start_frame":4,"loop_length_frames":80}}]
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
    banked.parameters.root_key = 60U;
    banked.parameters.key_low = 0U;
    banked.parameters.key_high = 127U;
    volume.samples.push_back(std::move(banked));

    axk::SampleSpec direct;
    direct.name = "Direct Sample";
    direct.waveform_id = "wave";
    direct.parameters.root_key = 60U;
    direct.parameters.key_low = 0U;
    direct.parameters.key_high = 127U;
    volume.samples.push_back(std::move(direct));
    volume.sample_banks.push_back({"Graph Bank", {"Grouped Sample"}});
    volume.programs.push_back({1U, "Pgm 001", {{"SBAC", "Graph Bank", 1U}, {"SBNK", "Direct Sample", 2U}}});
    return volume;
}

std::uint32_t read_be32(const std::vector<std::byte> &bytes, std::size_t offset) {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[offset + 3U]);
}

std::uint16_t read_be16(const std::vector<std::byte> &bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
                                      std::to_integer<std::uint16_t>(bytes[offset + 1U]));
}

} // namespace

TEST(HdsManifest, ParsesStrictSchemaAndResolvesRelativeAudioPaths) {
    const auto parsed = axk::parse_hds_build_manifest(manifest, "/project");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed->partitions.size(), 1U);
    EXPECT_EQ(parsed->partitions[0].volumes[0].waveforms[0].path, "/project/audio/tone.wav");
    EXPECT_EQ(parsed->partitions[0].volumes[0].waveforms[0].fine_tune_cents, -17);
    EXPECT_EQ(parsed->partitions[0].volumes[0].waveforms[0].loop_start_frame, 4U);
    EXPECT_EQ(parsed->partitions[0].volumes[0].samples[0].parameters.level.value_or(100U), 100U);
    EXPECT_EQ(parsed->partitions[0].volumes[0].samples[0].parameters.velocity_low, 8U);
    EXPECT_EQ(parsed->partitions[0].volumes[0].samples[0].parameters.velocity_high, 110U);
    EXPECT_EQ(parsed->partitions[0].volumes[0].samples[0].parameters.loop_length_frames, 80U);
    const axk::detail::PreparedWaveformMember member{"Tone", 0x100U, 44'100U, 100U};
    const auto parsed_payload = axk::detail::prepare_sbnk_payload(parsed->partitions[0].volumes[0].samples[0], member);
    ASSERT_TRUE(parsed_payload) << parsed_payload.error().message;
    EXPECT_EQ(std::to_integer<std::uint8_t>((*parsed_payload)[0x116U]), 100U);

    axk::SampleSpec direct;
    direct.name = "Direct";
    const auto direct_payload = axk::detail::prepare_sbnk_payload(direct, member);
    ASSERT_TRUE(direct_payload) << direct_payload.error().message;
    EXPECT_EQ(std::to_integer<std::uint8_t>((*direct_payload)[0x116U]), 100U);
    const auto geometry = axk::plan_hds_geometry(*parsed);
    ASSERT_TRUE(geometry);
    ASSERT_EQ(geometry->size(), 1U);
    EXPECT_EQ((*geometry)[0].start_sector, 3U);
    EXPECT_EQ((*geometry)[0].first_payload_cluster, 488U);
}

TEST(HdsManifest, ParsesExpandedMonoAllLoopModesAndOriginalKeyLimits) {
    constexpr std::string_view expanded = R"json({
      "schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"P1","volumes":[{
        "name":"V1","waveforms":[{"id":"wave","name":"Wave","path":"wave.wav","root_key":60}],
        "samples":[{"name":"Expanded","waveform_id":"wave","parameters":{
                    "root_key":60,"key_low":255,"key_high":128,"expand_detune":-5,
                    "expand_dephase":27,"expand_width":-41,"loop_mode":5,
                    "loop_start_frame":17,"loop_length_frames":80}}]
      }]}]
    })json";

    const auto parsed = axk::parse_hds_build_manifest(expanded);

    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto &sample = parsed->partitions[0].volumes[0].samples[0];
    EXPECT_EQ(sample.parameters.key_low, axk::sampler_original_key_low_limit);
    EXPECT_EQ(sample.parameters.key_high, axk::sampler_original_key_high_limit);
    EXPECT_EQ(sample.parameters.expand_detune, -5);
    EXPECT_EQ(sample.parameters.expand_dephase, 27);
    EXPECT_EQ(sample.parameters.expand_width, -41);
    EXPECT_EQ(sample.parameters.loop_mode, axk::AudioSamplerLoopMode::reverse_one_shot);
}

TEST(HdsManifest, ParsesSemanticSampleBankParameterOverrides) {
    constexpr std::string_view source = R"json({
      "schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"P1","volumes":[{
        "name":"V1","waveforms":[{"id":"wave","name":"Wave","path":"wave.wav","root_key":64}],
        "samples":[
          {"name":"Banked","waveform_id":"wave","parameters":{"root_key":60,"key_low":0,"key_high":127}},
          {"name":"Direct","waveform_id":"wave","parameters":{
           "root_key":64,"key_low":255,"key_high":128,"level":87,"fine_tune_cents":-12,
           "velocity_low":20,"velocity_high":110,"expand_detune":-5,
           "expand_dephase":27,"expand_width":-41}}],
        "sample_banks":[{"name":"Bank","member_samples":["Banked"],"parameter_overrides":{
          "root_key":64,"key_low":255,"key_high":128,"level":87,"fine_tune_cents":-12,
          "velocity_low":20,"velocity_high":110,"expand_detune":-5,"expand_dephase":27,"expand_width":-41}}],
        "programs":[{"number":1,"name":"Program","assignments":[
          {"sample_bank":"Bank","receive_mode":"MIDI_CHANNEL","receive_channel":1},
          {"sample":"Direct","receive_mode":"MIDI_CHANNEL","receive_channel":2}]}]
      }]}]
    })json";

    const auto parsed = axk::parse_hds_build_manifest(source);

    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto &overrides = parsed->partitions[0].volumes[0].sample_banks[0].parameter_overrides;
    ASSERT_TRUE(overrides);
    EXPECT_EQ(overrides->root_key, 64U);
    EXPECT_EQ(overrides->key_low, axk::sampler_original_key_low_limit);
    EXPECT_EQ(overrides->key_high, axk::sampler_original_key_high_limit);
    EXPECT_EQ(overrides->fine_tune_cents, -12);
    EXPECT_EQ(overrides->expand_detune, -5);
    EXPECT_EQ(overrides->expand_dephase, 27);
    EXPECT_EQ(overrides->expand_width, -41);
}

TEST(HdsManifest, ParsesAllNestedSampleParameterGroupsAndRejectsNonContractFields) {
    constexpr std::string_view source = R"json({
      "schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"P1","volumes":[{
        "name":"V1","waveforms":[{"id":"wave","name":"Wave","path":"wave.wav","root_key":60}],
        "samples":[{"name":"Sample","waveform_id":"wave","parameters":{
          "fixed_pitch":true,"filter_cutoff":91,
          "feg":{"attack_rate":81,"level_velocity_sensitivity":-23},
          "peg":{"decay_rate":82,"range":13},
          "aeg":{"release_rate":83,"attack_mode":2},
          "lfo":{"speed":88,"pitch_mod_phase_invert":true},
          "controls":{"6":{"device":126,"function":36,"type":3,"range":-63}},
          "output2_destination":9,"portamento_rate":37}}]
      }]}]
    })json";

    const auto parsed = axk::parse_hds_build_manifest(source);

    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto &parameters = parsed->partitions[0].volumes[0].samples[0].parameters;
    EXPECT_EQ(parameters.fixed_pitch, true);
    EXPECT_EQ(parameters.filter_cutoff, 91U);
    EXPECT_EQ(parameters.feg.attack_rate, 81U);
    EXPECT_EQ(parameters.feg.level_velocity_sensitivity, -23);
    EXPECT_EQ(parameters.peg.decay_rate, 82U);
    EXPECT_EQ(parameters.peg.range, 13);
    EXPECT_EQ(parameters.aeg.release_rate, 83U);
    EXPECT_EQ(parameters.aeg.attack_mode, 2U);
    EXPECT_EQ(parameters.lfo.speed, 88U);
    EXPECT_EQ(parameters.lfo.pitch_mod_phase_invert, true);
    EXPECT_EQ(parameters.controls[5].device, 126U);
    EXPECT_EQ(parameters.controls[5].function, 36U);
    EXPECT_EQ(parameters.controls[5].type, 3U);
    EXPECT_EQ(parameters.controls[5].range, -63);
    EXPECT_EQ(parameters.output2_destination, 9U);
    EXPECT_EQ(parameters.portamento_rate, 37U);

    const auto manifest_with_parameters = [](std::string_view parameters) {
        return std::format(
            R"json({{"schema_version":"1.0","size_bytes":1048576,"partitions":[{{"name":"P1","volumes":[{{"name":"V1","waveforms":[{{"id":"wave","name":"Wave","path":"wave.wav","root_key":60}}],"samples":[{{"name":"Sample","waveform_id":"wave","parameters":{}}}]}}]}}]}})json",
            parameters);
    };
    for (const auto invalid : {R"json({"feg":{"range":12}})json", R"json({"peg":{"attack_mode":1}})json",
                               R"json({"aeg":{"init_level":1}})json", R"json({"feg":{}})json",
                               R"json({"controls":{"1":{}}})json", R"json({"sample_rate":44100})json",
                               R"json({"wave_start_frame":0})json", R"json({"sample_bank_member":true})json"}) {
        EXPECT_FALSE(axk::parse_hds_build_manifest(manifest_with_parameters(invalid))) << invalid;
    }
}

TEST(HdsManifest, RejectsExpandedMonoBankOverridesForInterleavedStereoMembers) {
    constexpr std::string_view source = R"json({
      "schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"P1","volumes":[{
        "name":"V1","waveforms":[{"id":"direct","name":"Direct Wave","path":"mono.wav","root_key":60}],
        "samples":[
          {"name":"Stereo","interleaved_audio_path":"stereo.wav","parameters":{"root_key":60,"key_low":0,"key_high":127}},
          {"name":"Direct","waveform_id":"direct","parameters":{"root_key":60,"key_low":0,"key_high":127}}],
        "sample_banks":[{"name":"Bank","member_samples":["Stereo"],
                         "parameter_overrides":{"expand_detune":1}}],
        "programs":[{"number":1,"name":"Program","assignments":[
          {"sample_bank":"Bank","receive_mode":"MIDI_CHANNEL","receive_channel":1},
          {"sample":"Direct","receive_mode":"MIDI_CHANNEL","receive_channel":2}]}]
      }]}]
    })json";

    EXPECT_FALSE(axk::parse_hds_build_manifest(source));
}

TEST(HdsManifest, RejectsSampleMembershipInMultipleSampleBanksForJsonAndTypedInputs) {
    constexpr std::string_view source = R"json({
      "schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"P1","volumes":[{
        "name":"V1","waveforms":[{"id":"wave","name":"Wave","path":"unused.wav","root_key":60}],
        "samples":[{"name":"Shared","waveform_id":"wave","parameters":{"root_key":60,"key_low":0,"key_high":127}}],
        "sample_banks":[
          {"name":"Bank 1","member_samples":["Shared"]},
          {"name":"Bank 2","member_samples":["Shared"]}],
        "programs":[]
      }]}]
    })json";
    EXPECT_FALSE(axk::parse_hds_build_manifest(source));

    axk::SampleSpec sample;
    sample.name = "Shared";
    sample.waveform_id = "wave";
    auto direct_1 = sample;
    direct_1.name = "Direct 1";
    auto direct_2 = sample;
    direct_2.name = "Direct 2";
    axk::VolumeSpec volume;
    volume.name = "V1";
    volume.samples.push_back(std::move(sample));
    volume.samples.push_back(std::move(direct_1));
    volume.samples.push_back(std::move(direct_2));
    volume.sample_banks = {{"Bank 1", {"Shared"}, {}}, {"Bank 2", {"Shared"}, {}}};
    volume.programs = {
        {1U,
         "Program1",
         {{"SBAC", "Bank 1", 1U, axk::ProgramReceiveMode::midi_channel},
          {"SBNK", "Direct 1", 2U, axk::ProgramReceiveMode::midi_channel}}},
        {2U,
         "Program2",
         {{"SBAC", "Bank 2", 1U, axk::ProgramReceiveMode::midi_channel},
          {"SBNK", "Direct 2", 2U, axk::ProgramReceiveMode::midi_channel}}},
    };
    axk::HdsBuildManifest manifest{
        std::string{axk::build_manifest_schema_version}, axk::minimum_hds_size, {{"P1", {std::move(volume)}}}};
    const auto output = std::filesystem::temp_directory_path() / "axklib-duplicate-bank-membership.hds";
    std::error_code error;
    std::filesystem::remove(output, error);

    const auto written = axk::write_hds_image(manifest, output);

    EXPECT_FALSE(written);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(HdsManifest, AcceptsPartitionsWithoutVolumes) {
    constexpr std::string_view empty = R"json({
      "schema_version":"1.0",
      "size_bytes":1048576,
      "partitions":[{"name":"P1","volumes":[]}]
    })json";
    const auto parsed = axk::parse_hds_build_manifest(empty);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed->partitions.size(), 1U);
    EXPECT_TRUE(parsed->partitions.front().volumes.empty());
}

TEST(HdsManifest, RejectsPartitionSupportDirectoryNameAsAVolume) {
    constexpr std::string_view reserved = R"json({
      "schema_version":"1.0",
      "size_bytes":1048576,
      "partitions":[{"name":"P1","volumes":[{
        "name":"PRF3   ","waveforms":[],"samples":[]
      }]}]
    })json";
    const auto parsed = axk::parse_hds_build_manifest(reserved);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, axk::ErrorCode::manifest_invalid);
    EXPECT_NE(parsed.error().message.find("reserved"), std::string::npos);
}

TEST(HdsManifest, RejectsObsoleteSampleAndSampleBankFields) {
    constexpr std::string_view obsolete = R"json({
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
    EXPECT_FALSE(axk::parse_hds_build_manifest(obsolete, "/project"));

    constexpr std::string_view singular_member = R"json({
      "schema_version":"1.0",
      "size_bytes":1048576,
      "partitions":[{"name":"P1","volumes":[{
        "name":"V1",
        "waveforms":[{"id":"wave","name":"Wave","path":"wave.wav","root_key":60}],
        "samples":[{"name":"Sample","waveform_id":"wave","parameters":{"root_key":60,"key_low":0,"key_high":127}}],
        "sample_banks":[{"name":"Bank","member_sample":"Sample"}]
      }]}]
    })json";
    EXPECT_FALSE(axk::parse_hds_build_manifest(singular_member, "/project"));

    constexpr std::string_view canonical_single_member = R"json({
      "schema_version":"1.0",
      "size_bytes":1048576,
      "partitions":[{"name":"P1","volumes":[{
        "name":"V1",
        "waveforms":[{"id":"wave","name":"Wave","path":"wave.wav","root_key":60}],
        "samples":[{"name":"Sample","waveform_id":"wave","parameters":{"root_key":60,"key_low":0,"key_high":127}}],
        "sample_banks":[{"name":"Bank","member_samples":["Sample"]}]
      }]}]
    })json";
    EXPECT_TRUE(axk::parse_hds_build_manifest(canonical_single_member, "/project"));

    constexpr std::string_view unsupported_version = R"json({
      "schema_version":"1.1",
      "size_bytes":1048576,
      "partitions":[{"name":"P1","volumes":[]}]
    })json";
    const auto rejected = axk::parse_hds_build_manifest(unsupported_version);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message, "manifest.schema_version must be '1.0'");
}

TEST(HdsManifest, RejectsUnknownFieldsReferencesAndInvalidGeometry) {
    auto unknown = std::string{manifest};
    unknown.replace(unknown.find("\"size_bytes\""), 12, "\"unknown\"");
    EXPECT_FALSE(axk::parse_hds_build_manifest(unknown));

    auto reference = std::string{manifest};
    reference.replace(reference.find("\"waveform_id\":\"tone\""), 20, "\"waveform_id\":\"missing\"");
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
    EXPECT_EQ(parsed_hds->schema_version, axk::build_manifest_schema_version);
    ASSERT_EQ(parsed_hds->partitions.size(), 1U);
    EXPECT_TRUE(parsed_hds->partitions.front().volumes.empty());
    EXPECT_EQ(parsed_hds->size_bytes, 536'870'912U);

    const auto floppy = axk::serialize_build_manifest_template(axk::BuildManifestKind::fat12_floppy);
    ASSERT_TRUE(floppy) << floppy.error().message;
    const auto parsed_floppy = axk::parse_media_build_manifest(*floppy, "manifest-root");
    ASSERT_TRUE(parsed_floppy) << parsed_floppy.error().message;
    EXPECT_EQ(parsed_floppy->schema_version, axk::build_manifest_schema_version);
    ASSERT_TRUE(parsed_floppy->authored_volume);
    EXPECT_EQ(parsed_floppy->format, axk::MediaImageFormat::fat12_floppy);
    ASSERT_EQ(parsed_floppy->authored_volume->waveforms.size(), 1U);
    EXPECT_EQ(parsed_floppy->authored_volume->waveforms.front().path.filename(), "tone.wav");

    const auto iso = axk::serialize_build_manifest_template(axk::BuildManifestKind::iso9660);
    ASSERT_TRUE(iso) << iso.error().message;
    const auto parsed_iso = axk::parse_media_build_manifest(*iso);
    ASSERT_TRUE(parsed_iso) << parsed_iso.error().message;
    EXPECT_EQ(parsed_iso->schema_version, axk::build_manifest_schema_version);
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
        axk::HdsBuildManifest value{"1.0", count == 1 ? axk::minimum_hds_size : axk::maximum_hds_size, {}};
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

    const auto payload = axk::detail::prepare_smpl_payload(waveform, audio, 0x100U, "Test Volume");
    ASSERT_FALSE(payload);
    EXPECT_EQ(payload.error().code, axk::ErrorCode::audio_wave_data_too_large);

    audio.output_frames = 1U;
    audio.pcm_channels = {
        std::vector<std::byte>(static_cast<std::size_t>(axk::maximum_wave_data_pcm16_bytes_per_channel + 2U))};
    const auto oversized_pcm = axk::detail::prepare_smpl_payload(waveform, audio, 0x100U, "Test Volume");
    ASSERT_FALSE(oversized_pcm);
    EXPECT_EQ(oversized_pcm.error().code, axk::ErrorCode::audio_wave_data_too_large);
}

TEST(AudioImport, SerializesMappedSamplerMetadataIntoWaveDataAndSamplePayloads) {
    axk::ImportedAudio audio;
    audio.output_sample_rate = 44'100U;
    audio.output_sample_width_bits = axk::sampler_output_sample_width_bits;
    audio.output_frames = 400U;
    audio.pcm_channels = {std::vector<std::byte>(800U)};

    axk::WaveformSpec waveform;
    waveform.name = "Mapped Wave";
    waveform.root_key = 64U;
    waveform.fine_tune_cents = 25;
    waveform.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
    waveform.loop_start_frame = 17U;
    waveform.loop_length_frames = 335U;
    const auto waveform_payload = axk::detail::prepare_smpl_payload(waveform, audio, 0x100U, "Mapped Volume");
    ASSERT_TRUE(waveform_payload) << waveform_payload.error().message;
    for (std::size_t offset = 0x43U; offset <= 0x49U; ++offset)
        EXPECT_EQ((*waveform_payload)[offset], std::byte{}) << offset;
    EXPECT_EQ((*waveform_payload)[0x68U], (*waveform_payload)[0x74U]);
    EXPECT_EQ((*waveform_payload)[0x69U], (*waveform_payload)[0x75U]);
    EXPECT_EQ((*waveform_payload)[0x6aU], (*waveform_payload)[0x76U]);
    EXPECT_EQ((*waveform_payload)[0x6bU], (*waveform_payload)[0x77U]);
    for (std::size_t offset = 0x68U; offset <= 0x6bU; ++offset)
        EXPECT_EQ((*waveform_payload)[offset], std::byte{}) << offset;
    EXPECT_EQ((*waveform_payload)[0x6cU], (*waveform_payload)[0x78U]);
    EXPECT_EQ((*waveform_payload)[0x6dU], (*waveform_payload)[0x79U]);
    EXPECT_EQ((*waveform_payload)[0x6eU], (*waveform_payload)[0x7aU]);
    for (std::size_t offset = 0x6fU; offset <= 0x73U; ++offset)
        EXPECT_EQ((*waveform_payload)[offset], std::byte{}) << offset;
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(waveform_payload->data() + 0x54U), 16U), "Mapped Volume   ");
    for (std::size_t offset = 0x74U; offset <= 0x77U; ++offset)
        EXPECT_EQ((*waveform_payload)[offset], std::byte{}) << offset;
    EXPECT_EQ((*waveform_payload)[0x84U], std::byte{0x30});
    EXPECT_EQ((*waveform_payload)[0xaaU], std::byte{});
    EXPECT_EQ((*waveform_payload)[0xabU], std::byte{});
    const auto decoded_waveform = axk::decode_object(*waveform_payload);
    ASSERT_TRUE(decoded_waveform) << decoded_waveform.error().message;
    const auto *smpl = std::get_if<axk::CurrentSmpl>(&decoded_waveform->payload);
    ASSERT_NE(smpl, nullptr);
    EXPECT_EQ(smpl->embedded_container_name.value, "Mapped Volume");
    EXPECT_EQ(smpl->pcm_transfer_control.value, 0x30U);
    EXPECT_EQ(smpl->pcm_transfer_format_selector, 0x30U);
    EXPECT_EQ(smpl->root_key.value, 64U);
    EXPECT_EQ(smpl->fine_tune_cents.value, 25);
    EXPECT_EQ(smpl->loop_mode.value, 1U);
    EXPECT_EQ(smpl->wave_start_frame.value, 0U);
    EXPECT_EQ(smpl->wave_length_frames.value, 400U);
    EXPECT_EQ(smpl->loop_start_frame.value, 17U);
    EXPECT_EQ(smpl->loop_length_frames.value, 335U);

    axk::SampleSpec sample;
    sample.name = "Mapped Sample";
    sample.parameters.root_key = 64U;
    sample.parameters.fine_tune_cents = 25;
    sample.parameters.key_low = 24U;
    sample.parameters.key_high = 96U;
    sample.parameters.velocity_low = 10U;
    sample.parameters.velocity_high = 110U;
    sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
    sample.parameters.loop_start_frame = 17U;
    sample.parameters.loop_length_frames = 335U;
    const axk::detail::PreparedWaveformMember member{"Mapped Wave", 0x100U, 44'100U, 400U};
    const auto sample_payload = axk::detail::prepare_sbnk_payload(sample, member);
    ASSERT_TRUE(sample_payload) << sample_payload.error().message;
    EXPECT_TRUE(std::ranges::all_of(std::span{*sample_payload}.subspan(0x43U, 7U),
                                    [](std::byte value) { return value == std::byte{}; }));
    EXPECT_TRUE(std::ranges::all_of(std::span{*sample_payload}.subspan(0x4aU, 0x22U),
                                    [](std::byte value) { return value == std::byte{}; }));
    EXPECT_TRUE(std::ranges::equal(std::span{*sample_payload}.subspan(0x6cU, 3U),
                                   std::span{*sample_payload}.subspan(0x78U, 3U)));
    EXPECT_TRUE(std::ranges::all_of(std::span{*sample_payload}.subspan(0x6fU, 9U),
                                    [](std::byte value) { return value == std::byte{}; }));
    EXPECT_EQ(read_be32(*sample_payload, 0x98U), 0U);
    EXPECT_EQ(read_be32(*sample_payload, 0x9cU), 0U);
    const auto decoded_sample = axk::decode_object(*sample_payload);
    ASSERT_TRUE(decoded_sample) << decoded_sample.error().message;
    const auto *sbnk = std::get_if<axk::CurrentSbnk>(&decoded_sample->payload);
    ASSERT_NE(sbnk, nullptr);
    EXPECT_TRUE(sbnk->common.transient_name_hash_alias_matches);
    EXPECT_TRUE(sbnk->common.body_prefix_alias_matches);
    EXPECT_EQ(sbnk->left.root_key, 64U);
    EXPECT_EQ(sbnk->left.fine_tune_cents, 25);
    EXPECT_EQ(sbnk->key_range_low, 24U);
    EXPECT_EQ(sbnk->key_range_high, 96U);
    EXPECT_EQ(sbnk->velocity_range_low, 10U);
    EXPECT_EQ(sbnk->velocity_range_high, 110U);
    EXPECT_EQ(sbnk->loop_mode, 1U);
    EXPECT_EQ(sbnk->left.wave_start_frame, 0U);
    EXPECT_EQ(sbnk->left.wave_length_frames, 400U);
    EXPECT_EQ(sbnk->left.loop_start_frame, 17U);
    EXPECT_EQ(sbnk->left.loop_length_frames, 335U);
    EXPECT_EQ(read_be32(*sample_payload, 0x15cU), 400U);
    EXPECT_EQ(read_be32(*sample_payload, 0x160U), 352U);
}

TEST(AudioImport, SerializesWaveDataReferenceValuesWithoutAnInventedBase) {
    axk::ImportedAudio audio;
    audio.output_sample_rate = 44'100U;
    audio.output_sample_width_bits = axk::sampler_output_sample_width_bits;
    audio.output_frames = 1U;
    audio.pcm_channels = {{std::byte{}, std::byte{}}};
    axk::WaveformSpec waveform;
    waveform.name = "Small Reference";

    const auto payload = axk::detail::prepare_smpl_payload(waveform, audio, 1U, "Test Volume");

    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_EQ((*payload)[0x6cU], std::byte{});
    EXPECT_EQ((*payload)[0x6dU], std::byte{});
    EXPECT_EQ((*payload)[0x6eU], std::byte{});
    EXPECT_EQ((*payload)[0x6fU], std::byte{});
    EXPECT_EQ((*payload)[0x78U], std::byte{});
    EXPECT_EQ((*payload)[0x79U], std::byte{});
    EXPECT_EQ((*payload)[0x7aU], std::byte{});
    EXPECT_EQ((*payload)[0x7bU], std::byte{1});
}

TEST(AudioImport, SerializesMissingWavLoopAsHardwareProvenForwardOneShot) {
    axk::ImportedAudio audio;
    audio.output_sample_rate = 44'100U;
    audio.output_sample_width_bits = axk::sampler_output_sample_width_bits;
    audio.output_frames = 400U;
    audio.pcm_channels = {std::vector<std::byte>(800U)};

    axk::WaveformSpec waveform;
    waveform.name = "One Shot Wave";
    const auto waveform_payload = axk::detail::prepare_smpl_payload(waveform, audio, 0x100U, "Test Volume");
    ASSERT_TRUE(waveform_payload) << waveform_payload.error().message;
    const auto decoded_waveform = axk::decode_object(*waveform_payload);
    ASSERT_TRUE(decoded_waveform) << decoded_waveform.error().message;
    const auto *smpl = std::get_if<axk::CurrentSmpl>(&decoded_waveform->payload);
    ASSERT_NE(smpl, nullptr);
    EXPECT_EQ(smpl->loop_mode.value, 4U);
    EXPECT_EQ(smpl->loop_start_frame.value, 0U);
    EXPECT_EQ(smpl->loop_length_frames.value, 400U);

    axk::SampleSpec sample;
    sample.name = "One Shot Sample";
    const axk::detail::PreparedWaveformMember member{"One Shot Wave", 0x100U, 44'100U, 400U};
    const auto sample_payload = axk::detail::prepare_sbnk_payload(sample, member);
    ASSERT_TRUE(sample_payload) << sample_payload.error().message;
    const auto decoded_sample = axk::decode_object(*sample_payload);
    ASSERT_TRUE(decoded_sample) << decoded_sample.error().message;
    const auto *sbnk = std::get_if<axk::CurrentSbnk>(&decoded_sample->payload);
    ASSERT_NE(sbnk, nullptr);
    EXPECT_EQ(sbnk->loop_mode, 4U);
    EXPECT_EQ(sbnk->left.wave_start_frame, 0U);
    EXPECT_EQ(sbnk->left.loop_start_frame, 0U);
    EXPECT_EQ(sbnk->left.loop_length_frames, 400U);
}

TEST(AudioImport, SerializesTheCompleteCanonicalFreshSampleParameterBlock) {
    axk::SampleSpec sample;
    sample.name = "Default Sample";
    const axk::detail::PreparedWaveformMember member{"Wave", 0x100U, 44'100U, 400U};

    const auto payload = axk::detail::prepare_sbnk_payload(sample, member);

    ASSERT_TRUE(payload) << payload.error().message;
    std::array<std::byte, 0xe0> expected{};
    const auto put_be16 = [&](std::size_t offset, std::uint16_t value) {
        expected[offset] = static_cast<std::byte>(value >> 8U);
        expected[offset + 1U] = static_cast<std::byte>(value);
    };
    const auto put_be32 = [&](std::size_t offset, std::uint32_t value) {
        expected[offset] = static_cast<std::byte>(value >> 24U);
        expected[offset + 1U] = static_cast<std::byte>(value >> 16U);
        expected[offset + 2U] = static_cast<std::byte>(value >> 8U);
        expected[offset + 3U] = static_cast<std::byte>(value);
    };
    constexpr std::array<std::byte, 24> controls{
        std::byte{0x4a}, std::byte{0x04}, std::byte{0x01}, std::byte{0x20}, std::byte{0x47}, std::byte{0x05},
        std::byte{0x01}, std::byte{0x20}, std::byte{0x49}, std::byte{0x0b}, std::byte{0x01}, std::byte{0xe0},
        std::byte{0x48}, std::byte{0x0c}, std::byte{0x01}, std::byte{0xe0}, std::byte{0},    std::byte{0},
        std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0}};
    std::ranges::copy(controls, expected.begin());
    expected[0x28U] = std::byte{2};
    expected[0x2cU] = std::byte{2};
    expected[0x2eU] = std::byte{60};
    put_be16(0x30U, 44'100U);
    put_be16(0x36U, axk::detail::sample_pitch_word(60U, 0, 44'100U));
    expected[0x3aU] = std::byte{127};
    expected[0x3cU] = std::byte{0x30};
    expected[0x3dU] = std::byte{4};
    put_be16(0x3eU, 9000U);
    put_be32(0x48U, 400U);
    put_be32(0x58U, 400U);
    const std::array<std::pair<std::size_t, std::uint8_t>, 18> value_defaults{{
        {0x62U, 127U},
        {0x63U, 4U},
        {0x65U, 127U},
        {0x6cU, 63U},
        {0x6eU, 100U},
        {0x72U, 127U},
        {0x75U, 127U},
        {0x76U, 127U},
        {0x77U, 127U},
        {0x7aU, 26U},
        {0x7bU, 64U},
        {0x7cU, 10U},
        {0x7eU, 127U},
        {0x7fU, 127U},
        {0x80U, 127U},
        {0x89U, 127U},
        {0x8aU, 127U},
        {0x8bU, 127U},
    }};
    for (const auto &[offset, value] : value_defaults)
        expected[offset] = static_cast<std::byte>(value);
    expected[0x93U] = std::byte{12};
    expected[0x94U] = std::byte{127};
    expected[0x95U] = std::byte{127};
    expected[0x96U] = std::byte{126};
    expected[0x99U] = std::byte{127};
    expected[0x9eU] = std::byte{1};
    expected[0x9fU] = std::byte{39};
    expected[0xa1U] = std::byte{1};
    constexpr std::array eq_coefficients{
        std::byte{0xc1}, std::byte{0xe0}, std::byte{0x1e}, std::byte{0x3a}, std::byte{0x20},
        std::byte{0x00}, std::byte{0x3e}, std::byte{0x20}, std::byte{0xe1}, std::byte{0xc6},
    };
    std::ranges::copy(eq_coefficients, expected.begin() + 0xaaU);
    put_be32(0xb4U, 400U);
    put_be32(0xb8U, 400U);
    std::ranges::copy(controls, expected.begin() + 0xbcU);
    expected[0xd6U] = std::byte{1};
    expected[0xd7U] = std::byte{127};
    expected[0xd9U] = std::byte{127};
    expected[0xdbU] = std::byte{90};
    expected[0xdcU] = std::byte{90};

    EXPECT_TRUE(std::ranges::equal(expected, std::span{*payload}.subspan(0xa8U, expected.size())));
}

TEST(AudioImport, SerializesSampleTopologyFlagsFromMemberCountAndBankMembership) {
    axk::SampleSpec sample;
    sample.name = "Topology Sample";
    const axk::detail::PreparedWaveformMember left{"Wave L", 0x100U, 44'100U, 400U};
    const std::optional<axk::detail::PreparedWaveformMember> right{
        axk::detail::PreparedWaveformMember{"Wave R", 0x200U, 44'100U, 400U}};

    const auto standalone_mono = axk::detail::prepare_sbnk_payload(sample, left);
    const auto banked_mono = axk::detail::prepare_sbnk_payload(sample, left, {}, true);
    const auto standalone_stereo = axk::detail::prepare_sbnk_payload(sample, left, right);
    const auto banked_stereo = axk::detail::prepare_sbnk_payload(sample, left, right, true);

    ASSERT_TRUE(standalone_mono) << standalone_mono.error().message;
    ASSERT_TRUE(banked_mono) << banked_mono.error().message;
    ASSERT_TRUE(standalone_stereo) << standalone_stereo.error().message;
    ASSERT_TRUE(banked_stereo) << banked_stereo.error().message;
    EXPECT_EQ((*standalone_mono)[0xd0U], std::byte{0x02});
    EXPECT_EQ((*banked_mono)[0xd0U], std::byte{0x03});
    EXPECT_EQ((*standalone_stereo)[0xd0U], std::byte{0x00});
    EXPECT_EQ((*banked_stereo)[0xd0U], std::byte{0x01});
}

TEST(AudioImport, SerializesEveryProvenSampleLoopModeWithIndependentWaveAndLoopWindows) {
    constexpr std::array modes{
        axk::AudioSamplerLoopMode::forward,
        axk::AudioSamplerLoopMode::forward_loop,
        axk::AudioSamplerLoopMode::forward_loop_release,
        axk::AudioSamplerLoopMode::reverse,
        axk::AudioSamplerLoopMode::forward_one_shot,
        axk::AudioSamplerLoopMode::reverse_one_shot,
    };
    const axk::detail::PreparedWaveformMember member{"Loop Wave", 0x100U, 44'100U, 400U};
    for (const auto mode : modes) {
        axk::SampleSpec sample;
        sample.name = "Loop Sample";
        sample.parameters.loop_mode = mode;
        sample.parameters.loop_start_frame = 17U;
        sample.parameters.loop_length_frames = 335U;

        const auto payload = axk::detail::prepare_sbnk_payload(sample, member);

        ASSERT_TRUE(payload) << payload.error().message;
        EXPECT_EQ((*payload)[0xe5U], static_cast<std::byte>(mode));
        EXPECT_EQ(read_be32(*payload, 0xe8U), 0U);
        EXPECT_EQ(read_be32(*payload, 0xecU), 0U);
        EXPECT_EQ(read_be32(*payload, 0xf0U), 400U);
        EXPECT_EQ(read_be32(*payload, 0xf8U), 17U);
        EXPECT_EQ(read_be32(*payload, 0x100U), 335U);
        EXPECT_EQ(read_be32(*payload, 0x15cU), 400U);
        EXPECT_EQ(read_be32(*payload, 0x160U), 352U);
    }
}

TEST(AudioImport, DerivesExpandedMonoTopologyFromSemanticExpandControls) {
    axk::SampleSpec sample;
    sample.name = "Expanded Sample";
    sample.parameters.expand_detune = -5;
    sample.parameters.expand_dephase = 27;
    sample.parameters.expand_width = -41;
    const axk::detail::PreparedWaveformMember left{"Wave L", 0x100U, 44'100U, 400U};
    const std::optional<axk::detail::PreparedWaveformMember> right{
        axk::detail::PreparedWaveformMember{"Wave R", 0x200U, 44'100U, 400U}};

    const auto standalone = axk::detail::prepare_sbnk_payload(sample, left);
    const auto banked = axk::detail::prepare_sbnk_payload(sample, left, {}, true);

    ASSERT_TRUE(standalone) << standalone.error().message;
    ASSERT_TRUE(banked) << banked.error().message;
    EXPECT_EQ((*standalone)[0xd0U], std::byte{0x06});
    EXPECT_EQ((*banked)[0xd0U], std::byte{0x07});
    EXPECT_EQ((*standalone)[0x112U], std::byte{0xfb});
    EXPECT_EQ((*standalone)[0x113U], std::byte{0x1b});
    EXPECT_EQ((*standalone)[0x114U], std::byte{0xd7});
    EXPECT_FALSE(axk::detail::prepare_sbnk_payload(sample, left, right));
}

TEST(AudioImport, SerializesTheSharedWritableSampleParameterModel) {
    axk::SampleSpec sample;
    sample.name = "Parameter Sample";
    sample.parameters.fixed_pitch = true;
    sample.parameters.key_crossfade = true;
    sample.parameters.mono_mode = true;
    sample.parameters.sample_eq_type = 2U;
    sample.parameters.midi_receive_channel = 16U;
    sample.parameters.pitch_bend_type = 12U;
    sample.parameters.pitch_bend_range = 24U;
    sample.parameters.coarse_tune = -12;
    sample.parameters.root_key = 64U;
    sample.parameters.fine_tune_cents = -17;
    sample.parameters.key_low = 12U;
    sample.parameters.key_high = 96U;
    sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
    sample.parameters.loop_tempo_hundredths = 12'340U;
    sample.parameters.loop_start_frame = 17U;
    sample.parameters.loop_length_frames = 335U;
    sample.parameters.wave_start_velocity_sensitivity = -37;
    sample.parameters.filter_type = 15U;
    sample.parameters.filter_cutoff = 91U;
    sample.parameters.filter_q_width = 23U;
    sample.parameters.filter_scaling_break1 = 36U;
    sample.parameters.filter_scaling_break2 = 96U;
    sample.parameters.filter_scaling_cutoff1 = -45;
    sample.parameters.filter_scaling_cutoff2 = 52;
    sample.parameters.filter_velocity_to_cutoff = 68;
    sample.parameters.filter_velocity_to_q_width = -31;
    sample.parameters.expand_detune = -5;
    sample.parameters.expand_dephase = 27;
    sample.parameters.expand_width = -41;
    sample.parameters.random_pitch = 31U;
    sample.parameters.level = 87U;
    sample.parameters.pan = -17;
    sample.parameters.velocity_low_limit = 9U;
    sample.parameters.velocity_offset = -23;
    sample.parameters.velocity_high = 110U;
    sample.parameters.velocity_low = 20U;
    sample.parameters.level_scaling_break1 = 40U;
    sample.parameters.level_scaling_break2 = 100U;
    sample.parameters.level_scaling_level1 = 72U;
    sample.parameters.level_scaling_level2 = 83U;
    sample.parameters.velocity_sensitivity = 44;
    sample.parameters.alternate_group = 16U;
    sample.parameters.sample_eq_frequency = 37U;
    sample.parameters.sample_eq_gain_db = -7;
    sample.parameters.sample_eq_width_tenths = 44U;
    sample.parameters.filter_cutoff_distance = -19;
    sample.parameters.feg.attack_rate = 81U;
    sample.parameters.feg.decay_rate = 82U;
    sample.parameters.feg.release_rate = 83U;
    sample.parameters.feg.init_level = -61;
    sample.parameters.feg.attack_level = -31;
    sample.parameters.feg.sustain_level = 17;
    sample.parameters.feg.release_level = 63;
    sample.parameters.feg.rate_key_scaling = -7;
    sample.parameters.feg.rate_velocity_sensitivity = -51;
    sample.parameters.feg.attack_level_velocity_sensitivity = 42;
    sample.parameters.feg.level_velocity_sensitivity = -23;
    sample.parameters.peg.attack_rate = 71U;
    sample.parameters.peg.decay_rate = 72U;
    sample.parameters.peg.release_rate = 73U;
    sample.parameters.peg.init_level = -52;
    sample.parameters.peg.attack_level = -22;
    sample.parameters.peg.sustain_level = 18;
    sample.parameters.peg.release_level = 62;
    sample.parameters.peg.rate_key_scaling = -6;
    sample.parameters.peg.rate_velocity_sensitivity = -41;
    sample.parameters.peg.level_velocity_sensitivity = 31;
    sample.parameters.peg.range = 13;
    sample.parameters.aeg.attack_rate = 61U;
    sample.parameters.aeg.decay_rate = 62U;
    sample.parameters.aeg.release_rate = 63U;
    sample.parameters.aeg.sustain_level = 84U;
    sample.parameters.aeg.attack_mode = 2U;
    sample.parameters.aeg.rate_key_scaling = -5;
    sample.parameters.aeg.rate_velocity_sensitivity = -31;
    sample.parameters.lfo.wave = 3U;
    sample.parameters.lfo.speed = 88U;
    sample.parameters.lfo.delay_time = 89U;
    sample.parameters.lfo.key_on_sync = false;
    sample.parameters.lfo.cutoff_mod_phase_invert = true;
    sample.parameters.lfo.pitch_mod_phase_invert = true;
    sample.parameters.lfo.cutoff_mod_depth = 90U;
    sample.parameters.lfo.pitch_mod_depth = 91U;
    sample.parameters.lfo.amp_mod_depth = 92U;
    sample.parameters.filter_gain = -13;
    sample.parameters.controls = {
        axk::SampleControlParameters{.device = 65U, .function = 36U, .type = 3U, .range = -63},
        axk::SampleControlParameters{.device = 66U, .function = 35U, .type = 2U, .range = -42},
        axk::SampleControlParameters{.device = 67U, .function = 34U, .type = 1U, .range = -21},
        axk::SampleControlParameters{.device = 68U, .function = 33U, .type = 0U, .range = 0},
        axk::SampleControlParameters{.device = 69U, .function = 32U, .type = 1U, .range = 21},
        axk::SampleControlParameters{.device = 70U, .function = 31U, .type = 2U, .range = 42},
    };
    sample.parameters.velocity_xfade_high = 96U;
    sample.parameters.velocity_xfade_low = 32U;
    sample.parameters.output1_destination = 12U;
    sample.parameters.output1_level = 90U;
    sample.parameters.output2_destination = 9U;
    sample.parameters.output2_level = 45U;
    sample.parameters.portamento_type = 1U;
    sample.parameters.portamento_rate = 37U;
    sample.parameters.portamento_time = 91U;
    const axk::detail::PreparedWaveformMember member{"Wave", 0x100U, 44'100U, 400U};

    const auto payload = axk::detail::prepare_sbnk_payload(sample, member);

    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_EQ((*payload)[0xd1U], std::byte{0x97});
    EXPECT_EQ((*payload)[0xd2U], std::byte{16});
    EXPECT_EQ((*payload)[0xd3U], std::byte{12});
    EXPECT_EQ((*payload)[0xd4U], std::byte{24});
    EXPECT_EQ((*payload)[0xd5U], std::byte{0xf4});
    EXPECT_EQ((*payload)[0xd6U], std::byte{64});
    EXPECT_EQ((*payload)[0xdcU], std::byte{0xef});
    EXPECT_EQ((*payload)[0xe2U], std::byte{96});
    EXPECT_EQ((*payload)[0xe3U], std::byte{12});
    EXPECT_EQ((*payload)[0xe5U], std::byte{1});
    EXPECT_EQ(read_be16(*payload, 0xe6U), 12'340U);
    EXPECT_EQ((*payload)[0x108U], std::byte{0xdb});
    EXPECT_EQ((*payload)[0x109U], std::byte{15});
    EXPECT_EQ((*payload)[0x10cU], std::byte{36});
    EXPECT_EQ((*payload)[0x10eU], std::byte{0xd3});
    EXPECT_EQ((*payload)[0x110U], std::byte{68});
    EXPECT_EQ((*payload)[0x115U], std::byte{31});
    EXPECT_EQ((*payload)[0x117U], std::byte{0xef});
    EXPECT_EQ((*payload)[0x122U], std::byte{37});
    EXPECT_EQ((*payload)[0x123U], std::byte{57});
    EXPECT_EQ((*payload)[0x124U], std::byte{44});
    EXPECT_EQ((*payload)[0x126U], std::byte{81});
    EXPECT_EQ((*payload)[0x127U], std::byte{82});
    EXPECT_EQ((*payload)[0x128U], std::byte{83});
    EXPECT_EQ((*payload)[0x129U], std::byte{0xc3});
    EXPECT_EQ((*payload)[0x12aU], std::byte{0xe1});
    EXPECT_EQ((*payload)[0x12bU], std::byte{17});
    EXPECT_EQ((*payload)[0x12cU], std::byte{63});
    EXPECT_EQ((*payload)[0x12dU], std::byte{0xf9});
    EXPECT_EQ((*payload)[0x12eU], std::byte{0xcd});
    EXPECT_EQ((*payload)[0x12fU], std::byte{42});
    EXPECT_EQ((*payload)[0x130U], std::byte{0xe9});
    EXPECT_EQ((*payload)[0x131U], std::byte{71});
    EXPECT_EQ((*payload)[0x132U], std::byte{72});
    EXPECT_EQ((*payload)[0x133U], std::byte{73});
    EXPECT_EQ((*payload)[0x134U], std::byte{0xcc});
    EXPECT_EQ((*payload)[0x135U], std::byte{0xea});
    EXPECT_EQ((*payload)[0x136U], std::byte{18});
    EXPECT_EQ((*payload)[0x137U], std::byte{62});
    EXPECT_EQ((*payload)[0x138U], std::byte{0xfa});
    EXPECT_EQ((*payload)[0x139U], std::byte{0xd7});
    EXPECT_EQ((*payload)[0x13aU], std::byte{31});
    EXPECT_EQ((*payload)[0x13bU], std::byte{13});
    EXPECT_EQ((*payload)[0x13cU], std::byte{61});
    EXPECT_EQ((*payload)[0x13dU], std::byte{62});
    EXPECT_EQ((*payload)[0x13eU], std::byte{63});
    EXPECT_EQ((*payload)[0x141U], std::byte{84});
    EXPECT_EQ((*payload)[0x143U], std::byte{2});
    EXPECT_EQ((*payload)[0x144U], std::byte{0xfb});
    EXPECT_EQ((*payload)[0x145U], std::byte{0xe1});
    EXPECT_EQ((*payload)[0x146U], std::byte{3});
    EXPECT_EQ((*payload)[0x147U], std::byte{87});
    EXPECT_EQ((*payload)[0x149U], std::byte{0x06});
    EXPECT_EQ((*payload)[0x14aU], std::byte{90});
    EXPECT_EQ((*payload)[0x151U], std::byte{0xf3});
    const std::array<std::array<std::uint8_t, 4>, 6> controls{{
        {65U, 36U, 3U, 0xc1U},
        {66U, 35U, 2U, 0xd6U},
        {67U, 34U, 1U, 0xebU},
        {68U, 33U, 0U, 0U},
        {69U, 32U, 1U, 21U},
        {70U, 31U, 2U, 42U},
    }};
    for (std::size_t index = 0; index < controls.size(); ++index) {
        for (std::size_t field = 0; field < controls[index].size(); ++field) {
            EXPECT_EQ((*payload)[0xa8U + index * 4U + field], static_cast<std::byte>(controls[index][field]));
            EXPECT_EQ((*payload)[0x164U + index * 4U + field], static_cast<std::byte>(controls[index][field]));
        }
    }
    EXPECT_EQ((*payload)[0x17cU], std::byte{96});
    EXPECT_EQ((*payload)[0x17dU], std::byte{32});
    EXPECT_EQ((*payload)[0x17eU], std::byte{12});
    EXPECT_EQ((*payload)[0x17fU], std::byte{90});
    EXPECT_EQ((*payload)[0x180U], std::byte{9});
    EXPECT_EQ((*payload)[0x181U], std::byte{45});
    EXPECT_EQ((*payload)[0x182U], std::byte{1});
    EXPECT_EQ((*payload)[0x183U], std::byte{37});
    EXPECT_EQ((*payload)[0x184U], std::byte{91});
}

TEST(AudioImport, DerivesSampleEqBiquadCoefficientsFromSemanticParameters) {
    axk::SampleSpec sample;
    sample.name = "High Shelf";
    sample.parameters.sample_eq_type = 2U;
    sample.parameters.sample_eq_frequency = 51U;
    sample.parameters.sample_eq_gain_db = 5;
    sample.parameters.sample_eq_width_tenths = 10U;
    const axk::detail::PreparedWaveformMember member{"Wave", 0x100U, 44'100U, 400U};
    constexpr std::array expected{
        std::byte{0xd8}, std::byte{0x40}, std::byte{0x08}, std::byte{0x59}, std::byte{0x2f},
        std::byte{0x50}, std::byte{0x12}, std::byte{0xdf}, std::byte{0xfd}, std::byte{0x38},
    };

    const auto payload = axk::detail::prepare_sbnk_payload(sample, member);

    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_TRUE(std::ranges::equal(expected, std::span{*payload}.subspan(0x152U, expected.size())));
}

TEST(AudioImport, AppliesSampleEqPeakWidthAndShelfGainRules) {
    const auto prepare = [](std::uint8_t type, std::uint8_t frequency, std::int8_t gain, std::uint8_t width) {
        axk::SampleSpec sample;
        sample.name = "EQ";
        sample.parameters.sample_eq_type = type;
        sample.parameters.sample_eq_frequency = frequency;
        sample.parameters.sample_eq_gain_db = gain;
        sample.parameters.sample_eq_width_tenths = width;
        return axk::detail::prepare_sbnk_payload(sample, {"Wave", 0x100U, 44'100U, 400U});
    };
    const auto coefficients = [](const std::vector<std::byte> &payload) {
        std::array<std::int32_t, 5> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            const auto raw = read_be16(payload, 0x152U + index * 2U);
            result[index] = raw > static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())
                                ? static_cast<std::int32_t>(raw) - 65'536
                                : static_cast<std::int32_t>(raw);
        }
        return result;
    };

    const auto observed_peak = prepare(0U, 27U, 0, 10U);
    const auto wide_peak = prepare(0U, 27U, 6, 120U);
    const auto narrow_peak = prepare(0U, 27U, 6, 10U);
    const auto narrow_low_shelf = prepare(1U, 37U, 6, 10U);
    const auto wide_low_shelf = prepare(1U, 37U, 6, 120U);
    const auto limited_high_shelf = prepare(2U, 30U, 12, 10U);
    const auto explicit_high_shelf_limit = prepare(2U, 30U, 6, 10U);

    ASSERT_TRUE(observed_peak);
    ASSERT_TRUE(wide_peak);
    ASSERT_TRUE(narrow_peak);
    ASSERT_TRUE(narrow_low_shelf);
    ASSERT_TRUE(wide_low_shelf);
    ASSERT_TRUE(limited_high_shelf);
    ASSERT_TRUE(explicit_high_shelf_limit);
    constexpr std::array<std::int32_t, 5> stored_peak{-15'843, 7'684, 8'192, 15'843, -7'684};
    const auto derived_peak = coefficients(*observed_peak);
    for (std::size_t index = 0; index < stored_peak.size(); ++index)
        EXPECT_LE(std::abs(derived_peak[index] - stored_peak[index]), 1);
    EXPECT_NE(coefficients(*wide_peak), coefficients(*narrow_peak));
    EXPECT_EQ(coefficients(*narrow_low_shelf), coefficients(*wide_low_shelf));
    EXPECT_EQ(coefficients(*limited_high_shelf), coefficients(*explicit_high_shelf_limit));
}

TEST(AudioImport, SerializesOriginalKeyRangeSentinelsAndSpacePaddedNames) {
    axk::SampleSpec sample;
    sample.name = "Pad";
    sample.parameters.root_key = 60U;
    sample.parameters.key_low = axk::sampler_original_key_low_limit;
    sample.parameters.key_high = axk::sampler_original_key_high_limit;
    const axk::detail::PreparedWaveformMember member{"Wave", 0x100U, 44'100U, 400U};

    const auto payload = axk::detail::prepare_sbnk_payload(sample, member);

    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_EQ((*payload)[0xe2U], std::byte{0x80});
    EXPECT_EQ((*payload)[0xe3U], std::byte{0xff});
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(payload->data() + 0x32U), 16U), "Pad             ");
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(payload->data() + 0x78U), 16U), "Wave            ");
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

TEST(HdsWriter, WritesCompleteMatchingAllocationBitmapsBeyondFirst4096Clusters) {
    axk::Waveform source;
    source.format = {1, 2, 44'100};
    source.frame_count = 2'200'000U;
    source.pcm.resize(source.frame_count * 2U);
    const auto audio_path = std::filesystem::temp_directory_path() / "axklib-writer-large-bitmap.wav";
    const auto image_path = std::filesystem::temp_directory_path() / "axklib-writer-large-bitmap.hds";
    std::error_code error;
    std::filesystem::remove(audio_path, error);
    std::filesystem::remove(image_path, error);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, source));

    axk::VolumeSpec volume;
    volume.name = "Large Bitmap";
    volume.waveforms.push_back({"wave", "Large Wave", audio_path, 60U, {}});
    axk::HdsBuildManifest manifest_value{"1.0", 16U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};
    const auto written = axk::write_hds_image(manifest_value, image_path);
    ASSERT_TRUE(written) << written.error().message;
    ASSERT_EQ(written->partitions.size(), 1U);
    const auto &geometry = written->partitions.front().geometry;
    const auto bitmap_size = static_cast<std::size_t>(geometry.bitmap_cluster_count * 1024U);
    ASSERT_GT(bitmap_size, 512U);

    const auto read_bitmap = [&](std::uint64_t offset) {
        std::ifstream stream{image_path, std::ios::binary};
        std::vector<std::byte> bytes(bitmap_size);
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        EXPECT_TRUE(stream);
        return bytes;
    };
    const auto partition_start = geometry.start_sector * 512U;
    const auto fixed_location = read_bitmap(partition_start + 2048U);
    const auto header_addressed = read_bitmap(partition_start + geometry.bitmap_cluster * 1024U);
    EXPECT_TRUE(std::ranges::any_of(std::span{header_addressed}.subspan(512U),
                                    [](std::byte value) { return value != std::byte{}; }));
    EXPECT_EQ(fixed_location, header_addressed);

    std::filesystem::remove(audio_path, error);
    std::filesystem::remove(image_path, error);
}

TEST(HdsWriter, RejectsPartitionSupportDirectoryNameAsAVolume) {
    axk::VolumeSpec reserved;
    reserved.name = "PRF3";
    axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {{"hd1", {std::move(reserved)}}}};
    const auto path = std::filesystem::temp_directory_path() / "axklib-native-reserved-prf3.hds";
    std::error_code error;
    std::filesystem::remove(path, error);

    const auto written = axk::write_hds_image(manifest_value, path);
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().code, axk::ErrorCode::manifest_invalid);
    EXPECT_NE(written.error().message.find("reserved"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(HdsWriter, SizesDirectoryIndexFromRecordIdsAndEnforcesFixedCapacity) {
    const auto record = [](std::uint32_t id) {
        return axk::detail::PreparedRecord{id, {}, axk::detail::RecordKind::directory};
    };
    const std::array first_page{record(0U), record(13U)};
    const auto first_size = axk::detail::checked_directory_index_size(first_page);
    ASSERT_TRUE(first_size) << first_size.error().message;
    EXPECT_EQ(*first_size, 1024U);

    const std::array second_page{record(0U), record(14U)};
    const auto second_size = axk::detail::checked_directory_index_size(second_page);
    ASSERT_TRUE(second_size) << second_size.error().message;
    EXPECT_EQ(*second_size, 2048U);

    const std::array boundary{record(0U), record(5011U)};
    const auto boundary_size = axk::detail::checked_directory_index_size(boundary);
    ASSERT_TRUE(boundary_size) << boundary_size.error().message;
    EXPECT_EQ(*boundary_size, 358U * 1024U);

    const std::array outside{record(0U), record(5012U)};
    EXPECT_FALSE(axk::detail::checked_directory_index_size(outside));
    const std::array duplicate{record(1U), record(1U)};
    EXPECT_FALSE(axk::detail::checked_directory_index_size(duplicate));
}

TEST(HdsWriter, RejectsAValidTypedManifestThatExceedsTheFixedDirectoryIndex) {
    axk::HdsBuildManifest value{"1.0", axk::maximum_hds_size, {{"hd1", {}}}};
    for (std::size_t index = 0; index < 835U; ++index)
        value.partitions.front().volumes.push_back({"V" + std::to_string(index), {}, {}, {}, {}});
    const auto geometry = axk::plan_hds_geometry(value);
    ASSERT_TRUE(geometry) << geometry.error().message;
    const auto records = axk::detail::prepare_partition_records(value.partitions.front(), geometry->front(), 1U, {});
    ASSERT_FALSE(records);
    EXPECT_EQ(records.error().code, axk::ErrorCode::unsupported_profile);
}

TEST(HdsWriter, DirectSampleBankPreparationSupportsOneToOneHundredTwentySevenUniqueMembers) {
    std::map<std::string, axk::SampleSpec> samples;
    std::vector<std::string> member_names;
    for (std::size_t index = 1U; index <= 128U; ++index) {
        auto name = std::format("Sample {}", index);
        axk::SampleSpec sample;
        sample.name = name;
        samples.emplace(name, std::move(sample));
        member_names.push_back(std::move(name));
    }
    EXPECT_FALSE(axk::detail::prepare_sbac_payload({"Empty", {}, {}}, samples));
    EXPECT_FALSE(axk::detail::prepare_sbac_payload({"Duplicate", {"Sample 1", "Sample 1"}, {}}, samples));
    axk::SampleBankSpec empty_overrides{"Empty Overrides", {"Sample 1"}, {}};
    empty_overrides.parameter_overrides.emplace();
    EXPECT_FALSE(axk::detail::prepare_sbac_payload(empty_overrides, samples));

    const std::vector<std::string> eight_members{member_names.begin(), member_names.begin() + 8};
    const auto eight = axk::detail::prepare_sbac_payload({"Eight", eight_members, {}}, samples);
    ASSERT_TRUE(eight) << eight.error().message;
    EXPECT_EQ(eight->size(), 0x210U);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(eight->data() + 0x32U), 16U), "Eight           ");
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(eight->data() + 0x14cU), 16U), "Sample 1        ");
    EXPECT_EQ((*eight)[0x78U], std::byte{0x4a});
    EXPECT_EQ((*eight)[0x79U], std::byte{0x04});
    EXPECT_EQ((*eight)[0xa4U], std::byte{0x02});
    EXPECT_EQ((*eight)[0xa6U], std::byte{0x3c});
    EXPECT_EQ((*eight)[0xa7U], std::byte{0x3c});
    EXPECT_EQ((*eight)[0xb2U], std::byte{0x7f});
    EXPECT_EQ((*eight)[0xb3U], std::byte{0x00});
    EXPECT_EQ((*eight)[0xb4U], std::byte{0x30});
    EXPECT_EQ((*eight)[0xb6U], std::byte{0x23});
    EXPECT_EQ((*eight)[0xb7U], std::byte{0x28});
    EXPECT_EQ((*eight)[0xdaU], std::byte{0x7f});
    EXPECT_EQ((*eight)[0xe4U], std::byte{0x3f});
    EXPECT_EQ((*eight)[0xe6U], std::byte{0x64});
    EXPECT_EQ((*eight)[0x132U], std::byte{0x00});
    EXPECT_EQ((*eight)[eight->size() - 0x24U + 0x1aU], std::byte{0x01});
    EXPECT_EQ((*eight)[eight->size() - 0x24U + 0x1bU], std::byte{0x7f});
    EXPECT_EQ((*eight)[eight->size() - 0x24U + 0x1fU], std::byte{0x5a});
    EXPECT_EQ((*eight)[eight->size() - 0x24U + 0x20U], std::byte{0x5a});

    const auto linked = axk::detail::prepare_sbac_payload({"Linked", eight_members, {}}, samples, {1U, 32U, 33U, 128U});
    ASSERT_TRUE(linked) << linked.error().message;
    EXPECT_EQ(read_be32(*linked, 0x90U), 0x80000001U);
    EXPECT_EQ(read_be32(*linked, 0x94U), 0x00000001U);
    EXPECT_EQ(read_be32(*linked, 0x98U), 0U);
    EXPECT_EQ(read_be32(*linked, 0x9cU), 0x80000000U);
    EXPECT_FALSE(axk::detail::prepare_sbac_payload({"Bad Link", eight_members, {}}, samples, {0U}));
    EXPECT_FALSE(axk::detail::prepare_sbac_payload({"Bad Link", eight_members, {}}, samples, {129U}));

    const std::vector<std::string> nine_members{member_names.begin(), member_names.begin() + 9};
    const auto nine = axk::detail::prepare_sbac_payload({"Nine", nine_members, {}}, samples);
    ASSERT_TRUE(nine) << nine.error().message;
    EXPECT_EQ(nine->size(), 0x224U);

    member_names.resize(127U);
    const auto maximum = axk::detail::prepare_sbac_payload({"Maximum", member_names, {}}, samples);
    ASSERT_TRUE(maximum) << maximum.error().message;
    EXPECT_EQ(maximum->size(), 0xb5cU);
    const auto decoded = axk::decode_object(*maximum);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->header.record_size_or_header_used, 0xb08U);
    EXPECT_EQ(decoded->header.payload_bytes_0x1c, 0xb2cU);
    const auto *sample_bank = std::get_if<axk::CurrentSbac>(&decoded->payload);
    ASSERT_NE(sample_bank, nullptr);
    ASSERT_EQ(sample_bank->slots.size(), 127U);
    EXPECT_EQ(sample_bank->stored_member_count, 127U);
    EXPECT_EQ(sample_bank->effective_member_count, 127U);
    EXPECT_EQ(sample_bank->pending_parameter_propagation_words, (std::array<std::uint32_t, 3>{0U, 0U, 0U}));
    EXPECT_TRUE(std::ranges::all_of(
        sample_bank->slots, [](const auto &slot) { return slot.active && slot.transient_member_pointer == 0U; }));
    EXPECT_EQ(sample_bank->slots.front().name, "Sample 1");
    EXPECT_EQ(sample_bank->slots.back().name, "Sample 127");

    member_names.push_back("Sample 128");
    EXPECT_FALSE(axk::detail::prepare_sbac_payload({"Oversized", member_names, {}}, samples));
}

TEST(HdsWriter, SerializesTheCompleteCanonicalFreshSampleBankParameterState) {
    axk::SampleSpec member;
    member.name = "Member";
    const std::map<std::string, axk::SampleSpec> samples{{member.name, member}};

    const auto payload = axk::detail::prepare_sbac_payload({"Bank", {member.name}, {}}, samples);

    ASSERT_TRUE(payload) << payload.error().message;
    std::array<std::byte, 0xe0> actual{};
    std::copy_n(payload->begin() + 0x78U, 0xbcU, actual.begin());
    std::copy_n(payload->end() - 0x24U, 0x24U, actual.begin() + 0xbcU);
    std::array<std::byte, 0xe0> expected{};
    const auto put_be16 = [&](std::size_t offset, std::uint16_t value) {
        expected[offset] = static_cast<std::byte>(value >> 8U);
        expected[offset + 1U] = static_cast<std::byte>(value);
    };
    constexpr std::array<std::byte, 16> controls{std::byte{0x4a}, std::byte{0x04}, std::byte{0x01}, std::byte{0x20},
                                                 std::byte{0x47}, std::byte{0x05}, std::byte{0x01}, std::byte{0x20},
                                                 std::byte{0x49}, std::byte{0x0b}, std::byte{0x01}, std::byte{0xe0},
                                                 std::byte{0x48}, std::byte{0x0c}, std::byte{0x01}, std::byte{0xe0}};
    std::ranges::copy(controls, expected.begin());
    expected[0x2cU] = std::byte{2};
    expected[0x2eU] = std::byte{60};
    expected[0x2fU] = std::byte{60};
    put_be16(0x30U, 44'100U);
    put_be16(0x32U, 44'100U);
    put_be16(0x36U, axk::detail::sample_pitch_word(60U, 0, 44'100U));
    put_be16(0x38U, axk::detail::sample_pitch_word(60U, 0, 44'100U));
    expected[0x3aU] = std::byte{127};
    expected[0x3cU] = std::byte{0x30};
    put_be16(0x3eU, 9000U);
    const std::array<std::pair<std::size_t, std::uint8_t>, 18> value_defaults{{
        {0x62U, 127U},
        {0x63U, 4U},
        {0x65U, 127U},
        {0x6cU, 63U},
        {0x6eU, 100U},
        {0x72U, 127U},
        {0x75U, 127U},
        {0x76U, 127U},
        {0x77U, 127U},
        {0x7aU, 26U},
        {0x7bU, 64U},
        {0x7cU, 10U},
        {0x7eU, 127U},
        {0x7fU, 127U},
        {0x80U, 127U},
        {0x89U, 127U},
        {0x8aU, 127U},
        {0x8bU, 127U},
    }};
    for (const auto &[offset, value] : value_defaults)
        expected[offset] = static_cast<std::byte>(value);
    expected[0x93U] = std::byte{12};
    expected[0x94U] = std::byte{127};
    expected[0x95U] = std::byte{127};
    expected[0x96U] = std::byte{126};
    expected[0x97U] = std::byte{8};
    expected[0x98U] = std::byte{127};
    expected[0x99U] = std::byte{127};
    expected[0x9eU] = std::byte{1};
    expected[0x9fU] = std::byte{39};
    expected[0xa1U] = std::byte{1};
    constexpr std::array eq_coefficients{
        std::byte{0xc1}, std::byte{0xe0}, std::byte{0x1e}, std::byte{0x3a}, std::byte{0x20},
        std::byte{0x00}, std::byte{0x3e}, std::byte{0x20}, std::byte{0xe1}, std::byte{0xc6},
    };
    std::ranges::copy(eq_coefficients, expected.begin() + 0xaaU);
    std::ranges::copy(controls, expected.begin() + 0xbcU);
    expected[0xd6U] = std::byte{1};
    expected[0xd7U] = std::byte{127};
    expected[0xd9U] = std::byte{127};
    expected[0xdbU] = std::byte{90};
    expected[0xdcU] = std::byte{90};

    EXPECT_EQ(actual, expected);
}

TEST(HdsWriter, SerializesCanonicalSampleBankDefaultsAndSemanticOverrides) {
    axk::SampleSpec member;
    member.name = "Member";
    std::map<std::string, axk::SampleSpec> samples{{member.name, member}};
    axk::SampleBankSpec sample_bank{"Bank", {member.name}, {}};
    sample_bank.parameter_overrides.emplace();
    sample_bank.parameter_overrides->root_key = 64U;
    sample_bank.parameter_overrides->fine_tune_cents = -12;
    sample_bank.parameter_overrides->key_low = axk::sampler_original_key_low_limit;
    sample_bank.parameter_overrides->key_high = axk::sampler_original_key_high_limit;
    sample_bank.parameter_overrides->expand_detune = -5;
    sample_bank.parameter_overrides->expand_dephase = 27;
    sample_bank.parameter_overrides->expand_width = -41;
    sample_bank.parameter_overrides->level = 87U;
    sample_bank.parameter_overrides->velocity_high = 110U;
    sample_bank.parameter_overrides->velocity_low = 20U;

    const auto payload = axk::detail::prepare_sbac_payload(sample_bank, samples);

    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_EQ((*payload)[0xa6U], std::byte{64});
    EXPECT_EQ((*payload)[0xacU], std::byte{0xf4});
    EXPECT_EQ((*payload)[0xb2U], std::byte{0x80});
    EXPECT_EQ((*payload)[0xb3U], std::byte{0xff});
    EXPECT_EQ((*payload)[0xe2U], std::byte{0xfb});
    EXPECT_EQ((*payload)[0xe3U], std::byte{0x1b});
    EXPECT_EQ((*payload)[0xe4U], std::byte{0xd7});
    EXPECT_EQ((*payload)[0xe6U], std::byte{87});
    EXPECT_EQ((*payload)[0xeaU], std::byte{110});
    EXPECT_EQ((*payload)[0xebU], std::byte{20});
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto *bank = std::get_if<axk::CurrentSbac>(&decoded->payload);
    ASSERT_NE(bank, nullptr);
    EXPECT_TRUE(bank->pending_parameter_numbers.empty());
    EXPECT_TRUE(bank->reserved_pending_parameter_numbers.empty());
}

TEST(HdsWriter, SerializesSharedParametersIntoSampleBankState) {
    axk::SampleSpec member;
    member.name = "Member";
    std::map<std::string, axk::SampleSpec> samples{{member.name, member}};
    axk::SampleBankSpec sample_bank{"Bank", {member.name}, {}};
    sample_bank.parameter_overrides.emplace();
    auto &parameters = *sample_bank.parameter_overrides;
    parameters.fixed_pitch = true;
    parameters.sample_eq_type = 2U;
    parameters.sample_eq_frequency = 51U;
    parameters.sample_eq_gain_db = 5;
    parameters.sample_eq_width_tenths = 10U;
    parameters.filter_cutoff = 91U;
    parameters.feg.attack_rate = 73U;
    parameters.peg.range = -17;
    parameters.aeg.attack_mode = 2U;
    parameters.lfo.speed = 88U;
    parameters.filter_gain = -9;
    parameters.controls[5].device = 126U;
    parameters.controls[5].function = 36U;
    parameters.controls[5].type = 3U;
    parameters.controls[5].range = -63;
    parameters.velocity_xfade_high = 96U;
    parameters.output1_destination = 12U;
    parameters.output1_level = 90U;
    parameters.portamento_type = 1U;
    parameters.portamento_rate = 37U;
    parameters.portamento_time = 91U;

    const auto payload = axk::detail::prepare_sbac_payload(sample_bank, samples);

    ASSERT_TRUE(payload) << payload.error().message;
    constexpr std::array expected_eq{
        std::byte{0xd8}, std::byte{0x40}, std::byte{0x08}, std::byte{0x59}, std::byte{0x2f},
        std::byte{0x50}, std::byte{0x12}, std::byte{0xdf}, std::byte{0xfd}, std::byte{0x38},
    };
    EXPECT_EQ((*payload)[0xa1U], std::byte{0x91});
    EXPECT_TRUE(std::ranges::equal(expected_eq, std::span{*payload}.subspan(0x122U, expected_eq.size())));
    EXPECT_EQ((*payload)[0xdaU], std::byte{91});
    EXPECT_EQ((*payload)[0xf6U], std::byte{73});
    EXPECT_EQ((*payload)[0x10bU], std::byte{0xef});
    EXPECT_EQ((*payload)[0x113U], std::byte{2});
    EXPECT_EQ((*payload)[0x117U], std::byte{87});
    EXPECT_EQ((*payload)[0x121U], std::byte{0xf7});
    EXPECT_EQ((*payload)[0x8cU], std::byte{126});
    EXPECT_EQ((*payload)[0x8fU], std::byte{0xc1});
    EXPECT_EQ((*payload)[0x200U], std::byte{126});
    EXPECT_EQ((*payload)[0x203U], std::byte{0xc1});
    EXPECT_EQ((*payload)[0x204U], std::byte{96});
    EXPECT_EQ((*payload)[0x206U], std::byte{12});
    EXPECT_EQ((*payload)[0x207U], std::byte{90});
    EXPECT_EQ((*payload)[0x20aU], std::byte{1});
    EXPECT_EQ((*payload)[0x20bU], std::byte{37});
    EXPECT_EQ((*payload)[0x20cU], std::byte{91});
}

TEST(HdsWriter, SampleBankFineTuneOverridePreservesDistinctStereoRootKeys) {
    axk::SampleSpec sample;
    sample.name = "Stereo";
    sample.parameters.root_key = 60U;
    const axk::detail::PreparedWaveformMember left{"Left", 0x100U, 44'100U, 400U};
    const std::optional<axk::detail::PreparedWaveformMember> right{
        axk::detail::PreparedWaveformMember{"Right", 0x200U, 44'100U, 400U}};
    auto payload = axk::detail::prepare_sbnk_payload(sample, left, right);
    ASSERT_TRUE(payload) << payload.error().message;
    (*payload)[0xd7U] = std::byte{72};
    axk::SampleParameters overrides;
    overrides.fine_tune_cents = -12;

    const auto applied = axk::detail::apply_sample_parameters_to_payload(*payload, overrides);

    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ((*payload)[0xd6U], std::byte{60});
    EXPECT_EQ((*payload)[0xd7U], std::byte{72});
    EXPECT_EQ((*payload)[0xdcU], std::byte{0xf4});
    EXPECT_EQ((*payload)[0xddU], std::byte{0xf4});
    EXPECT_NE(read_be16(*payload, 0xdeU), read_be16(*payload, 0xe0U));
}

TEST(HdsWriter, AppliesSharedSampleBankParametersAndPreservesUnspecifiedBytes) {
    axk::SampleSpec sample;
    sample.name = "Member";
    sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
    sample.parameters.loop_start_frame = 10U;
    sample.parameters.loop_length_frames = 300U;
    const axk::detail::PreparedWaveformMember member{"Wave", 0x100U, 44'100U, 400U};
    auto payload = axk::detail::prepare_sbnk_payload(sample, member);
    ASSERT_TRUE(payload) << payload.error().message;
    (*payload)[0xd1U] = std::byte{0x20};
    (*payload)[0x14dU] = std::byte{0x5a};
    constexpr std::array stored_eq{
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67}, std::byte{0x10},
        std::byte{0x32}, std::byte{0x54}, std::byte{0x76}, std::byte{0x11}, std::byte{0x22},
    };
    std::ranges::copy(stored_eq, payload->begin() + 0x152U);
    const auto prefix = std::vector<std::byte>(payload->begin(), payload->begin() + 0xa8U);

    axk::SampleParameters overrides;
    overrides.fixed_pitch = true;
    overrides.root_key = 67U;
    overrides.loop_start_frame = 17U;
    overrides.loop_length_frames = 335U;
    overrides.filter_cutoff = 91U;
    overrides.lfo.speed = 88U;
    overrides.controls[0].range = -63;
    overrides.output1_level = 90U;
    overrides.portamento_type = 1U;

    const auto applied = axk::detail::apply_sample_parameters_to_payload(*payload, overrides);

    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(std::ranges::equal(prefix, std::span{*payload}.first(0xa8U)));
    EXPECT_EQ((*payload)[0xd1U], std::byte{0x31});
    EXPECT_EQ((*payload)[0xd6U], std::byte{67});
    EXPECT_EQ(read_be16(*payload, 0xdeU), axk::detail::sample_pitch_word(67U, 0, 44'100U));
    EXPECT_EQ(read_be32(*payload, 0xf8U), 17U);
    EXPECT_EQ(read_be32(*payload, 0x100U), 335U);
    EXPECT_EQ(read_be32(*payload, 0x160U), 352U);
    EXPECT_EQ((*payload)[0x10aU], std::byte{91});
    EXPECT_EQ((*payload)[0x147U], std::byte{87});
    EXPECT_EQ((*payload)[0x0abU], std::byte{0xc1});
    EXPECT_EQ((*payload)[0x167U], std::byte{0xc1});
    EXPECT_EQ((*payload)[0x17fU], std::byte{90});
    EXPECT_EQ((*payload)[0x182U], std::byte{1});
    EXPECT_EQ((*payload)[0x14dU], std::byte{0x5a});
    EXPECT_TRUE(std::ranges::equal(stored_eq, std::span{*payload}.subspan(0x152U, stored_eq.size())));
}

TEST(HdsWriter, RejectsExtendedOnlyOverrideForShortCurrentSample) {
    axk::SampleSpec sample;
    sample.name = "Short";
    const axk::detail::PreparedWaveformMember member{"Wave", 0x100U, 44'100U, 400U};
    auto payload = axk::detail::prepare_sbnk_payload(sample, member);
    ASSERT_TRUE(payload);
    payload->resize(0x164U);
    axk::SampleParameters overrides;
    overrides.output1_level = 90U;

    const auto applied = axk::detail::apply_sample_parameters_to_payload(*payload, overrides);

    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, axk::ErrorCode::transaction_rejected);
}

TEST(HdsWriter, AppliesSampleBankOverridesToFreshMemberSamples) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44'100};
    waveform.frame_count = 4U;
    waveform.pcm.resize(8U);
    const auto root = std::filesystem::temp_directory_path() / "axklib-bank-parameters";
    const auto audio_path = root / "tone.wav";
    const auto image_path = root / "parameters.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));

    auto volume = graph_volume(audio_path);
    auto &direct = volume.samples[1];
    direct.parameters.root_key = 64U;
    direct.parameters.key_low = axk::sampler_original_key_low_limit;
    direct.parameters.key_high = axk::sampler_original_key_high_limit;
    direct.parameters.level = 87U;
    auto &sample_bank = volume.sample_banks.front();
    sample_bank.parameter_overrides.emplace();
    sample_bank.parameter_overrides->root_key = direct.parameters.root_key;
    sample_bank.parameter_overrides->key_low = direct.parameters.key_low;
    sample_bank.parameter_overrides->key_high = direct.parameters.key_high;
    sample_bank.parameter_overrides->expand_detune = -5;
    sample_bank.parameter_overrides->expand_dephase = 27;
    sample_bank.parameter_overrides->expand_width = -41;
    sample_bank.parameter_overrides->level = direct.parameters.level;
    axk::HdsBuildManifest manifest_value{"1.0", 4U * 1024U * 1024U, {{"hd1", {std::move(volume)}}}};

    const auto written = axk::write_hds_image(manifest_value, image_path);

    ASSERT_TRUE(written) << written.error().message;
    const auto reopened = axk::open_media(image_path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto found = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbnk && object.object.header.name == "Grouped Sample";
    });
    ASSERT_NE(found, catalog->objects.end());
    const auto *sample = std::get_if<axk::CurrentSbnk>(&found->object.payload);
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->left.root_key, 64U);
    EXPECT_EQ(sample->key_range_low, axk::sampler_original_key_low_limit);
    EXPECT_EQ(sample->key_range_high, axk::sampler_original_key_high_limit);
    EXPECT_EQ(sample->sample_level, 87U);
    EXPECT_EQ(sample->sample_flags, 0x07U);
    EXPECT_EQ(found->raw_payload[0x112U], std::byte{0xfb});
    EXPECT_EQ(found->raw_payload[0x113U], std::byte{0x1b});
    EXPECT_EQ(found->raw_payload[0x114U], std::byte{0xd7});
    const auto found_bank = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbac && object.object.header.name == "Graph Bank";
    });
    ASSERT_NE(found_bank, catalog->objects.end());
    const auto *decoded_bank = std::get_if<axk::CurrentSbac>(&found_bank->object.payload);
    ASSERT_NE(decoded_bank, nullptr);
    EXPECT_EQ(decoded_bank->raw_sample_parameter_block[0x1bU], std::byte{1});
    std::filesystem::remove_all(root, error);
}

TEST(HdsWriter, PrivateObjectCodecsRejectValuesOutsideTheirEncodedFields) {
    axk::ImportedAudio audio;
    audio.output_sample_rate = 44'100U;
    audio.output_sample_width_bits = axk::sampler_output_sample_width_bits;
    audio.output_frames = 1U;
    audio.pcm_channels = {{std::byte{}, std::byte{}}};
    axk::WaveformSpec waveform;
    waveform.name = "Wave Data";
    audio.output_sample_rate = 0U;
    EXPECT_FALSE(axk::detail::prepare_smpl_payload(waveform, audio, 0x100U, "Test Volume"));
    audio.output_sample_rate = 44'100U;

    axk::SampleSpec sample;
    sample.name = "Sample";
    const axk::detail::PreparedWaveformMember member{"Wave Data", 0x100U, 44'100U, 1U};
    EXPECT_FALSE(axk::detail::prepare_sbnk_payload(sample, member, {}, false, {0U}));
    EXPECT_FALSE(axk::detail::prepare_sbnk_payload(sample, member, {}, false, {129U}));

    axk::ProgramSpec program;
    program.number = 1U;
    program.name = "Program";
    program.assignments.resize(axk::maximum_program_assignments + 1U);
    const auto oversized_program = axk::detail::prepare_prog_payload(program);
    ASSERT_FALSE(oversized_program);
    EXPECT_EQ(oversized_program.error().code, axk::ErrorCode::unsupported_profile);
}

TEST(HdsWriter, EncodesTheNativeTx16wProgramAssignmentCapacity) {
    axk::ProgramSpec program;
    program.number = 7U;
    program.name = "Wide";
    for (std::size_t index = 0U; index < axk::maximum_program_assignments; ++index) {
        program.assignments.push_back(
            {"SBAC", std::format("Bank {}", index + 1U), 0U, axk::ProgramReceiveMode::sample});
    }

    const auto payload = axk::detail::prepare_prog_payload(program);
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto *current = std::get_if<axk::CurrentProg>(&decoded->payload);
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->assignments.size(), axk::maximum_program_assignments);
    EXPECT_EQ(current->assignments.back().name, "Bank 16");
}

TEST(HdsWriter, EncodesSamplerControlledSingleTargetProgram) {
    axk::ProgramSpec program;
    program.number = 7U;
    program.name = "Bass";
    program.assignments.push_back({"SBAC", "Bass Bank", 0U, axk::ProgramReceiveMode::sample});

    const auto payload = axk::detail::prepare_prog_payload(program);
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto *current = std::get_if<axk::CurrentProg>(&decoded->payload);
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->program_name, "Bass");
    ASSERT_EQ(std::ranges::count_if(current->assignments, [](const auto &row) { return !row.name.empty(); }), 1U);
    EXPECT_EQ(current->assignments.front().name, "Bass Bank");
    EXPECT_EQ(current->assignments.front().kind, 0x11U);
    EXPECT_EQ(current->assignments.front().flags, 0xffU);
    EXPECT_EQ(current->assignments.front().raw_row[0x28], std::byte{0xff});
}

TEST(HdsWriter, AtomicallyWritesAndReopensPartitionWithoutVolumes) {
    axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {{"hd1", {}}}};
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
    sample.parameters.root_key = 60;
    sample.parameters.key_low = 48;
    sample.parameters.key_high = 72;
    sample.parameters.level = 96;
    axk::VolumeSpec volume;
    volume.name = "Volume";
    volume.waveforms.push_back(std::move(waveform));
    volume.samples.push_back(std::move(sample));
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
    const auto wave_data = std::ranges::find(catalog->objects, axk::ObjectType::smpl, type);
    ASSERT_NE(wave_data, catalog->objects.end());
    const auto decoded = axk::decode_waveform(*reopened, *wave_data);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::ranges::equal(source.pcm, std::span<const std::byte>{decoded->pcm}.first(source.pcm.size())));
    std::filesystem::remove(audio_path, error);
    std::filesystem::remove(image_path, error);
}

TEST(HdsWriter, CancellationPublishesNoImageOrTemporarySibling) {
    axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {}};
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

TEST(ImageBuildPlanning, PreparesEveryInputWithoutPublishingAnOutput) {
    axk::HdsBuildManifest hds{"1.0", axk::minimum_hds_size, {}};
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
    iso.schema_version = "1.0";
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
      "samples":[{
        "name":"Tone Sample","waveform_id":"tone","parameters":{"root_key":60,"key_low":0,"key_high":127}
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
    sample.parameters.root_key = 60;
    sample.parameters.key_high = 127;
    axk::VolumeSpec volume;
    volume.name = "Volume";
    volume.waveforms.push_back(std::move(waveform));
    volume.samples.push_back(std::move(sample));

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
    media_manifest.schema_version = "1.0";
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

TEST(MediaWriter, EnforcesAggregatePayloadAndProjectedOutputLimits) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-limits";
    const auto iso_path = root / "limited.iso";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::VolumeSpec empty_volume;
    empty_volume.name = "Empty";
    axk::MediaBuildManifest empty_iso;
    empty_iso.schema_version = "1.0";
    empty_iso.format = axk::MediaImageFormat::iso9660;
    empty_iso.authored_volume = empty_volume;
    empty_iso.iso_volume_id = "AXK_LIMIT";
    empty_iso.raw_group = "GROUP";
    empty_iso.group_name = "Limits";
    empty_iso.raw_volume = "F001";
    empty_iso.volume_name = "Empty";
    axk::MediaBuildLimits output_limited;
    output_limited.maximum_output_bytes = 4096U;
    const auto oversized_output = axk::write_media_image(empty_iso, iso_path, false, output_limited);
    ASSERT_FALSE(oversized_output);
    EXPECT_EQ(oversized_output.error().code, axk::ErrorCode::io_unsupported_size);
    EXPECT_FALSE(std::filesystem::exists(iso_path));

    axk::Waveform waveform;
    waveform.format = {1, 2, 44100};
    waveform.frame_count = 4U;
    waveform.pcm.resize(8U);
    const auto wav_path = root / "tone.wav";
    ASSERT_TRUE(axk::write_wav_atomic(wav_path, waveform));
    auto authored = empty_iso;
    authored.authored_volume->waveforms.push_back({"wave", "Wave", wav_path, 60U, {}});
    axk::MediaBuildLimits aggregate_limited;
    aggregate_limited.maximum_object_bytes = 128U;
    aggregate_limited.maximum_aggregate_payload_bytes = 128U;
    const auto oversized_payload = axk::plan_media_build(authored, aggregate_limited);
    ASSERT_FALSE(oversized_payload);
    EXPECT_EQ(oversized_payload.error().code, axk::ErrorCode::io_unsupported_size);

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
    axk::HdsBuildManifest hds{"1.0", 4U * 1024U * 1024U, {{"hd1", {volume}}}};
    ASSERT_TRUE(axk::write_hds_image(hds, source_path));
    const auto source_media = axk::open_media(source_path);
    ASSERT_TRUE(source_media);
    const auto source_objects = source_media->objects();
    ASSERT_TRUE(source_objects);
    const auto sample_object = std::ranges::find(*source_objects, axk::ObjectType::sbnk,
                                                 [](const auto &object) { return object.decoded.header.type; });
    ASSERT_NE(sample_object, source_objects->end());

    axk::MediaBuildManifest transfer;
    transfer.schema_version = "1.0";
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
    EXPECT_TRUE(std::filesystem::is_empty(root / ".axklib-publication"));
    std::filesystem::remove_all(root, error);
}

TEST(HdsWriter, PermissionLossLeavesNoTemporaryOutput) {
    if (::geteuid() == 0)
        GTEST_SKIP() << "permission checks are not meaningful as root";

    axk::HdsBuildManifest manifest_value{"1.0", axk::minimum_hds_size, {}};
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
