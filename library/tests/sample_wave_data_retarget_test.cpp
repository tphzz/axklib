#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {
using Json = nlohmann::json;

axk::Result<axk::AlterationManifest> parse_retarget(const Json &operation) {
    return axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
}

TEST(SamplePlaybackEditManifest, AcceptsWindowOnlyAndRejectsInvalidGuardsAndBounds) {
    const Json operation{{"id", "edit"},
                         {"type", "update_sbnk_parameters"},
                         {"partition_index", 0},
                         {"volume_name", "V"},
                         {"sample_name", "S"},
                         {"parameters", Json::object()},
                         {"playback_window", {{"start_frame", 1}, {"length_frames", 2}}},
                         {"expected_payload_sha256", std::string(64U, 'a')}};
    const auto parsed = parse_retarget(operation);
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto invalid = operation;
    invalid["expected_payload_sha256"] = "ABC";
    EXPECT_FALSE(parse_retarget(invalid));
    invalid = operation;
    invalid["playback_window"]["length_frames"] = 0;
    EXPECT_FALSE(parse_retarget(invalid));
    invalid = operation;
    invalid["playback_window"]["start_frame"] = 16777216;
    EXPECT_FALSE(parse_retarget(invalid));
    invalid = operation;
    invalid.erase("playback_window");
    EXPECT_FALSE(parse_retarget(invalid));
}

std::vector<char> image_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

const axk::ObjectSnapshot *find(const axk::ObjectCatalog &catalog, axk::ObjectType type, std::string_view name) {
    const auto found = std::ranges::find_if(catalog.objects, [&](const auto &object) {
        return object.object.header.type == type && object.object.header.name == name;
    });
    return found == catalog.objects.end() ? nullptr : &*found;
}

class SampleWaveDataRetarget : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;
    axk::ObjectCatalog before;

    static axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
        const auto image = axk::open_image(path);
        if (!image)
            return std::unexpected(image.error());
        return axk::build_object_catalog(*image);
    }

    void add_wave(axk::VolumeSpec &volume, std::string name, std::uint32_t rate, std::uint32_t frames) {
        axk::Waveform wave;
        wave.format = {1U, 2U, rate};
        wave.frame_count = frames;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            wave.pcm.push_back(static_cast<std::byte>(frame));
            wave.pcm.push_back(std::byte{0x40});
        }
        const auto audio = root / (name + ".wav");
        ASSERT_TRUE(axk::write_wav_atomic(audio, wave));
        axk::WaveformSpec spec{name, name, audio, 36U, {}};
        spec.loop_mode = axk::AudioSamplerLoopMode::forward;
        volume.waveforms.push_back(spec);
    }

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-sample-retarget-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
        for (std::size_t attempt = 0; attempt < 1024U; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() / (prefix + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = candidate;
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());
        source = root / "source.hds";
        output = root / "output.hds";
        axk::VolumeSpec volume;
        volume.name = "Retarget";
        add_wave(volume, "Old Left", 44100U, 16U);
        add_wave(volume, "Old Right", 44100U, 16U);
        add_wave(volume, "New Left", 32000U, 20U);
        add_wave(volume, "New Right", 32000U, 20U);
        add_wave(volume, "Short", 32000U, 4U);
        ASSERT_FALSE(HasFatalFailure());
        axk::SampleSpec sample;
        sample.name = "Mono";
        sample.waveform_id = "Old Left";
        sample.playback_window = axk::SamplePlaybackWindow{2U, 12U};
        sample.parameters.root_key = 60U;
        sample.parameters.fine_tune_cents = -7;
        sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
        sample.parameters.loop_start_frame = 4U;
        sample.parameters.loop_length_frames = 8U;
        sample.parameters.level = 83U;
        sample.parameters.sample_eq_type = 1U;
        sample.parameters.sample_eq_frequency = 30U;
        sample.parameters.sample_eq_gain_db = -6;
        volume.samples.push_back(sample);
        sample.name = "Stereo";
        sample.right_waveform_id = "Old Right";
        sample.parameters.root_key = 66U;
        sample.parameters.fine_tune_cents = 3;
        volume.samples.push_back(sample);
        sample.name = "Expanded";
        sample.right_waveform_id.reset();
        sample.parameters.expand_detune = 1;
        volume.samples.push_back(sample);
        volume.sample_banks.push_back({"Bank", {"Mono"}});
        volume.programs.push_back({1U, "Links", {{"SBAC", "Bank", {}}, {"SBNK", "Stereo", {}}}});
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(manifest, source);
        ASSERT_TRUE(written) << written.error().message;
        const auto objects = catalog(source);
        ASSERT_TRUE(objects);
        before = *objects;
        for (const auto *name : {"Mono", "Stereo", "Expanded"}) {
            patch_sample(name, 0x43U, {std::byte{0x5a}});
            patch_sample(name, 0x98U, {std::byte{0x5a}, std::byte{1}, std::byte{2}, std::byte{3}});
        }
        patch_sample("Stereo", 0x9cU, {std::byte{0x4b}, std::byte{4}, std::byte{5}, std::byte{6}});
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void patch_sample(std::string_view name, std::size_t offset, const std::vector<std::byte> &patch) {
        const auto *object = find(before, axk::ObjectType::sbnk, name);
        ASSERT_NE(object, nullptr);
        const auto image = image_bytes(source);
        const auto found =
            std::search(image.begin(), image.end(), object->raw_payload.begin(), object->raw_payload.end(),
                        [](char left, std::byte right) {
                            return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
                        });
        ASSERT_NE(found, image.end());
        {
            std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
            ASSERT_TRUE(file);
            file.seekp(static_cast<std::streamoff>(found - image.begin()) + static_cast<std::streamoff>(offset));
            for (const auto byte : patch)
                file.put(static_cast<char>(std::to_integer<unsigned char>(byte)));
            ASSERT_TRUE(file);
        }
        const auto objects = catalog(source);
        ASSERT_TRUE(objects);
        before = *objects;
    }

    Json operation(std::string_view sample_name, std::string_view left, std::string_view right = {}) const {
        const auto *sample = find(before, axk::ObjectType::sbnk, sample_name);
        const auto hash = sample ? axk::package_internal::hex_digest(axk::package_internal::sha256(sample->raw_payload))
                                 : std::string(64U, '0');
        Json result{{"id", "retarget"},
                    {"type", "retarget_sample_wave_data"},
                    {"partition_index", 0},
                    {"volume_name", "Retarget"},
                    {"sample_name", sample_name},
                    {"waveform_name", left},
                    {"expected_payload_sha256", hash}};
        if (!right.empty())
            result["right_waveform_name"] = right;
        return result;
    }

    void expect_exact_retarget(std::string_view sample_name, std::string_view left, std::string_view right = {}) {
        const auto parsed = parse_retarget(operation(sample_name, left, right));
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto original = image_bytes(source);
        const auto changed = axk::alter_hds(source, *parsed, output);
        ASSERT_TRUE(changed) << changed.error().message;
        EXPECT_EQ(image_bytes(source), original);
        const auto after = catalog(output);
        ASSERT_TRUE(after);
        ASSERT_EQ(before.objects.size(), after->objects.size());
        for (const auto &old : before.objects) {
            const auto current = std::ranges::find(after->objects, old.key, &axk::ObjectSnapshot::key);
            ASSERT_NE(current, after->objects.end());
            auto expected = old.raw_payload;
            if (old.object.header.type == axk::ObjectType::sbnk && old.object.header.name == sample_name) {
                const auto &sample = std::get<axk::CurrentSbnk>(old.object.payload);
                const bool matches =
                    std::ranges::equal(std::span{expected}.subspan(0x6cU, 3U), std::span{expected}.subspan(0x78U, 3U));
                for (const bool is_right : {false, true}) {
                    if (is_right && right.empty())
                        continue;
                    const auto name = is_right ? right : left;
                    const auto *target = find(before, axk::ObjectType::smpl, name);
                    ASSERT_NE(target, nullptr);
                    const auto &wave = std::get<axk::CurrentSmpl>(target->object.payload);
                    const auto &member = is_right ? *sample.right : sample.left;
                    const auto name_offset = is_right ? 0x88U : 0x78U;
                    std::fill_n(expected.begin() + name_offset, 16U, std::byte{' '});
                    for (std::size_t index = 0; index < name.size(); ++index)
                        expected[name_offset + index] = static_cast<std::byte>(name[index]);
                    axk::ByteWriter writer{expected};
                    ASSERT_TRUE(writer.write_be32(is_right ? 0xa4U : 0xa0U, wave.wave_data_reference_value.value));
                    if (member.wave_data_name != name) {
                        ASSERT_TRUE(writer.write_be32(is_right ? 0x9cU : 0x98U, 0U));
                    }
                    ASSERT_TRUE(writer.write_be16(is_right ? 0xdaU : 0xd8U, wave.sample_rate.value));
                    ASSERT_TRUE(writer.write_be16(is_right ? 0xe0U : 0xdeU, axk::detail::sample_pitch_word(
                                                                                member.root_key, member.fine_tune_cents,
                                                                                wave.sample_rate.value)));
                }
                if (matches)
                    std::copy_n(expected.begin() + 0x78U, 3U, expected.begin() + 0x6cU);
            }
            EXPECT_EQ(current->raw_payload, expected) << old.object.header.name;
        }
    }
};

TEST_F(SampleWaveDataRetarget, StrictManifestRejectsMissingGuardMalformedTargetsAndUnsupportedWindowOverride) {
    auto valid = operation("Mono", "New Left");
    EXPECT_TRUE(parse_retarget(valid));
    auto invalid = valid;
    invalid.erase("expected_payload_sha256");
    EXPECT_FALSE(parse_retarget(invalid));
    for (const auto &hash : {std::string(63U, 'a'), std::string(64U, 'A'), std::string(64U, 'g')}) {
        invalid = valid;
        invalid["expected_payload_sha256"] = hash;
        EXPECT_FALSE(parse_retarget(invalid));
    }
    invalid = valid;
    invalid["waveform_name"] = "";
    EXPECT_FALSE(parse_retarget(invalid));
    invalid = valid;
    invalid["playback_window"] = {{"start_frame", 0}, {"length_frames", 1}};
    EXPECT_FALSE(parse_retarget(invalid));
}

TEST_F(SampleWaveDataRetarget, MonoRetargetPreservesBankMembershipParametersEqWindowsAndAllPcm) {
    expect_exact_retarget("Mono", "New Left");
}

TEST_F(SampleWaveDataRetarget, CombinedWindowAndLoopEditPreservesEveryOtherByte) {
    for (const auto selector : {1U, 2U, 4U}) {
        for (const auto *name : {"Mono", "Stereo"}) {
            // Legacy-layout Samples use the same stored playback lanes.
            patch_sample(name, 0x14U, {std::byte{0}, std::byte{0}, std::byte{0}, static_cast<std::byte>(selector)});
            ASSERT_FALSE(HasFatalFailure());
            auto edit = operation(name, "Old Left");
            edit["type"] = "update_sbnk_parameters";
            edit.erase("waveform_name");
            edit["parameters"] = {{"loop_start_frame", 0}, {"loop_length_frames", 16}};
            edit["playback_window"] = {{"start_frame", 0}, {"length_frames", 16}};
            auto parsed = parse_retarget(edit);
            ASSERT_TRUE(parsed) << parsed.error().message;
            const auto destination = root / (std::string{name} + std::to_string(selector) + ".hds");
            auto changed = axk::alter_hds(source, *parsed, destination);
            ASSERT_TRUE(changed) << changed.error().message;
            const auto after = catalog(destination);
            ASSERT_TRUE(after);
            for (const auto &old : before.objects) {
                const auto *current = find(*after, old.object.header.type, old.object.header.name);
                ASSERT_NE(current, nullptr);
                auto expected = old.raw_payload;
                if (old.object.header.type == axk::ObjectType::sbnk && old.object.header.name == name) {
                    axk::ByteWriter writer{expected};
                    for (const auto offset : {0xe8U, 0xf8U})
                        ASSERT_TRUE(writer.write_be32(offset, 0));
                    for (const auto offset : {0xf0U, 0x100U, 0x15cU, 0x160U})
                        ASSERT_TRUE(writer.write_be32(offset, 16));
                    if (std::string_view{name} == "Stereo") {
                        for (const auto offset : {0xecU, 0xfcU})
                            ASSERT_TRUE(writer.write_be32(offset, 0));
                        for (const auto offset : {0xf4U, 0x104U})
                            ASSERT_TRUE(writer.write_be32(offset, 16));
                    }
                }
                EXPECT_EQ(current->raw_payload, expected) << old.object.header.name;
            }
        }
    }
}

TEST_F(SampleWaveDataRetarget, WindowUpdateRejectsRetainedLoopOutsideWindowAndStalePayload) {
    auto edit = operation("Mono", "Old Left");
    edit["type"] = "update_sbnk_parameters";
    edit.erase("waveform_name");
    edit["parameters"] = Json::object();
    edit["playback_window"] = {{"start_frame", 0}, {"length_frames", 5}};
    auto parsed = parse_retarget(edit);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *parsed));
    edit["playback_window"]["length_frames"] = 16;
    edit["expected_payload_sha256"] = std::string(64U, '0');
    parsed = parse_retarget(edit);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *parsed));
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(SampleWaveDataRetarget, StereoRetargetRefreshesBothRatesAndPitchCachesPreservingProgramLinksAndParameters) {
    expect_exact_retarget("Stereo", "New Left", "New Right");
}

TEST_F(SampleWaveDataRetarget, SingleSourceExpandedMonoRetargetPreservesItsExistingTopologyAndExpandParameters) {
    expect_exact_retarget("Expanded", "New Left");
}

TEST_F(SampleWaveDataRetarget, SameIdentityRetargetPreservesRuntimeHandles) {
    expect_exact_retarget("Stereo", "Old Left", "Old Right");
    EXPECT_EQ(image_bytes(output), image_bytes(source));
}

TEST_F(SampleWaveDataRetarget, DivergentCommonPrefixResidueIsNotRewrittenToMatchNewTarget) {
    patch_sample("Mono", 0x6cU, {std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}});
    expect_exact_retarget("Mono", "New Left");
}

TEST_F(SampleWaveDataRetarget, InvalidTargetsTopologyWindowsAndStaleGuardRollBackExistingDestination) {
    const auto original = image_bytes(source);
    std::filesystem::copy_file(source, output);
    std::vector<Json> invalid{operation("Mono", "Missing"),
                              operation("Mono", "Short"),
                              operation("Mono", "New Left", "New Right"),
                              operation("Stereo", "New Left"),
                              operation("Stereo", "New Left", "Old Right"),
                              operation("Stereo", "New Left", "New Left"),
                              operation("Stereo", "New Left", "Short")};
    auto stale = operation("Mono", "New Left");
    stale["expected_payload_sha256"] = std::string(64U, '0');
    invalid.push_back(stale);
    for (const auto &operation : invalid) {
        SCOPED_TRACE(operation.dump());
        const auto parsed = parse_retarget(operation);
        if (parsed) {
            EXPECT_FALSE(axk::alter_hds(source, *parsed, output, {}, nullptr, true));
        }
        EXPECT_EQ(image_bytes(source), original);
        EXPECT_EQ(image_bytes(output), original);
    }
    auto later = parse_retarget(operation("Mono", "New Left"));
    ASSERT_TRUE(later);
    later->operations.push_back({"missing", axk::DeleteProgramOperation{axk::PartitionIndex{0}, "Retarget", 128U}});
    EXPECT_FALSE(axk::alter_hds(source, *later, output, {}, nullptr, true));
    EXPECT_EQ(image_bytes(source), original);
    EXPECT_EQ(image_bytes(output), original);
}

} // namespace
