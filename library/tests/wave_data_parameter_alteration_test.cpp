#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include "axklib/writer.hpp"

namespace {
using Json = nlohmann::json;

Json wave_update(Json parameters = {{"root_key", 64}}) {
    return {{"id", "wave-parameters"}, {"type", "update_wave_data_parameters"},
            {"partition_index", 0},    {"volume_name", "Waves"},
            {"waveform_name", "Wave"}, {"parameters", std::move(parameters)}};
}

axk::Result<axk::AlterationManifest> parse_wave_update(const Json &operation) {
    return axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
}

std::vector<char> image_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

class WaveDataParameterAlteration : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-wave-parameters-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
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
        const auto audio = root / "tone.wav";
        axk::Waveform wave;
        wave.format = {1U, 2U, 44'100U};
        wave.frame_count = 16U;
        for (std::uint8_t index = 0; index < 16U; ++index) {
            wave.pcm.push_back(static_cast<std::byte>(index));
            wave.pcm.push_back(static_cast<std::byte>(0x40U + index));
        }
        ASSERT_TRUE(axk::write_wav_atomic(audio, wave));
        axk::VolumeSpec volume;
        volume.name = "Waves";
        axk::WaveformSpec waveform{"wave", "Wave", audio, 60U, {}};
        waveform.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
        waveform.loop_start_frame = 2U;
        waveform.loop_length_frames = 10U;
        volume.waveforms.push_back(waveform);
        axk::SampleSpec sample;
        sample.name = "Sample A";
        sample.waveform_id = "wave";
        sample.parameters.root_key = 55U;
        sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward;
        volume.samples.push_back(sample);
        sample.name = "Sample B";
        sample.parameters.root_key = 67U;
        volume.samples.push_back(sample);
        const axk::HdsBuildManifest build{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(build, source);
        ASSERT_TRUE(written) << written.error().message;
        patch_wave(0x43U, {std::byte{0x5a}});
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void patch_wave(std::size_t offset, const std::vector<std::byte> &patch) {
        const auto image = image_bytes(source);
        constexpr std::string_view magic{"FSFSDEV3SPLXSMPL"};
        const auto found = std::search(image.begin(), image.end(), magic.begin(), magic.end());
        ASSERT_NE(found, image.end());
        ASSERT_EQ(
            std::search(found + static_cast<std::ptrdiff_t>(magic.size()), image.end(), magic.begin(), magic.end()),
            image.end());
        std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(file);
        file.seekp(static_cast<std::streamoff>(found - image.begin()) + static_cast<std::streamoff>(offset));
        for (const auto byte : patch)
            file.put(static_cast<char>(std::to_integer<unsigned char>(byte)));
        ASSERT_TRUE(file);
    }

    static axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
        const auto image = axk::open_image(path);
        if (!image)
            return std::unexpected(image.error());
        return axk::build_object_catalog(*image);
    }

    static const axk::ObjectSnapshot *wave_object(const axk::ObjectCatalog &objects) {
        const auto found = std::ranges::find_if(
            objects.objects, [](const auto &object) { return object.object.header.type == axk::ObjectType::smpl; });
        return found == objects.objects.end() ? nullptr : &*found;
    }

    void expect_rejected_without_changes(const Json &parameters) {
        const auto parsed = parse_wave_update(wave_update(parameters));
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto before = image_bytes(source);
        EXPECT_FALSE(axk::alter_hds(source, *parsed, output));
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(image_bytes(source), before);
    }
};

TEST(WaveDataParameterManifest, AcceptsSparseValuesAndAllModesAndRejectsUnsupportedOrMalformedFields) {
    for (unsigned mode = 0U; mode < 6U; ++mode) {
        const auto parsed = parse_wave_update(wave_update({{"loop_mode", mode}}));
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_EQ(axk::operation_type_name(parsed->operations.front().data), "update_wave_data_parameters");
    }
    const std::vector<Json> invalid{Json::object(),
                                    Json(nullptr),
                                    Json{{"root_key", 128}},
                                    Json{{"root_key", -1}},
                                    Json{{"root_key", true}},
                                    Json{{"root_key", 60.5}},
                                    Json{{"fine_tune_cents", -64}},
                                    Json{{"fine_tune_cents", 64}},
                                    Json{{"loop_mode", 6}},
                                    Json{{"loop_mode", -1}},
                                    Json{{"wave_start_frame", -1}},
                                    Json{{"wave_length_frames", 4294967296ULL}},
                                    Json{{"loop_start_frame", true}},
                                    Json{{"loop_length_frames", 1.5}},
                                    Json{{"sample_rate", 48000}},
                                    Json{{"stored_sample_width_bytes", 1}},
                                    Json{{"pcm_transfer_control", 48}},
                                    Json{{"unknown", 1}}};
    for (const auto &parameters : invalid) {
        SCOPED_TRACE(parameters.dump());
        EXPECT_FALSE(parse_wave_update(wave_update(parameters)));
    }
    auto missing = wave_update();
    missing.erase("parameters");
    EXPECT_FALSE(parse_wave_update(missing));
    missing = wave_update();
    missing["waveform_name"] = "";
    EXPECT_FALSE(parse_wave_update(missing));
}

TEST_F(WaveDataParameterAlteration, UpdatesOnlyWaveMetadataAndPitchCachePreservingPcmAndBothReferencingSamples) {
    const auto parsed = parse_wave_update(wave_update({{"root_key", 64},
                                                       {"fine_tune_cents", -12},
                                                       {"wave_start_frame", 4},
                                                       {"wave_length_frames", 12},
                                                       {"loop_start_frame", 6},
                                                       {"loop_length_frames", 8}}));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = image_bytes(source);
    const auto before = catalog(source);
    ASSERT_TRUE(before);

    const auto changed = axk::alter_hds(source, *parsed, output);

    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(image_bytes(source), original);
    const auto after = catalog(output);
    ASSERT_TRUE(after);
    ASSERT_EQ(after->objects.size(), before->objects.size());
    for (const auto &old : before->objects) {
        const auto current = std::ranges::find(after->objects, old.key, &axk::ObjectSnapshot::key);
        ASSERT_NE(current, after->objects.end());
        auto expected = old.raw_payload;
        if (old.object.header.type == axk::ObjectType::smpl) {
            expected[0x7eU] = std::byte{64};
            expected[0x7fU] = std::byte{0xf4};
            axk::ByteWriter writer{expected};
            ASSERT_TRUE(writer.write_be16(0x80U, 0x150cU));
            ASSERT_TRUE(writer.write_be32(0x8eU, 4U));
            ASSERT_TRUE(writer.write_be32(0x92U, 12U));
            ASSERT_TRUE(writer.write_be32(0x96U, 6U));
            ASSERT_TRUE(writer.write_be32(0x9aU, 8U));
        }
        EXPECT_EQ(current->raw_payload, expected) << old.object.header.name;
    }
    const auto again = root / "again.hds";
    ASSERT_TRUE(axk::alter_hds(output, *parsed, again));
    EXPECT_EQ(image_bytes(output), image_bytes(again));
}

TEST_F(WaveDataParameterAlteration, SupportsAllSixLoopModesWithExplicitLogicalWindows) {
    for (unsigned mode = 0U; mode < 6U; ++mode) {
        SCOPED_TRACE(mode);
        const auto parsed = parse_wave_update(wave_update({{"loop_mode", mode},
                                                           {"wave_start_frame", 4},
                                                           {"wave_length_frames", 12},
                                                           {"loop_start_frame", 6},
                                                           {"loop_length_frames", 10}}));
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto changed = axk::alter_hds(source, *parsed, root / (std::to_string(mode) + ".hds"));
        ASSERT_TRUE(changed) << changed.error().message;
        const auto after = catalog(root / (std::to_string(mode) + ".hds"));
        ASSERT_TRUE(after);
        const auto *object = wave_object(*after);
        ASSERT_NE(object, nullptr);
        const auto *wave = std::get_if<axk::CurrentSmpl>(&object->object.payload);
        ASSERT_NE(wave, nullptr);
        EXPECT_EQ(wave->loop_mode.value, mode);
        EXPECT_EQ(wave->wave_start_frame.value, 4U);
        EXPECT_EQ(wave->wave_end_frame_exclusive, 16U);
        EXPECT_EQ(wave->loop_end_frame_exclusive, 16U);
    }
}

TEST_F(WaveDataParameterAlteration, AllowsZeroLoopOnlyForNonRepeatingModesAndAcceptsExactStoredEndpoint) {
    for (const unsigned mode : {0U, 3U, 4U, 5U}) {
        const auto parsed = parse_wave_update(wave_update({{"loop_mode", mode},
                                                           {"wave_start_frame", 4},
                                                           {"wave_length_frames", 16},
                                                           {"loop_start_frame", 0},
                                                           {"loop_length_frames", 0}}));
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto changed = axk::alter_hds(source, *parsed, root / (std::to_string(mode) + ".hds"));
        ASSERT_TRUE(changed) << changed.error().message;
        const auto after = catalog(root / (std::to_string(mode) + ".hds"));
        ASSERT_TRUE(after);
        const auto *object = wave_object(*after);
        ASSERT_NE(object, nullptr);
        const auto &wave = std::get<axk::CurrentSmpl>(object->object.payload);
        EXPECT_EQ(wave.wave_end_frame_exclusive, 20U);
        EXPECT_EQ(wave.stored_pcm_bytes, 40U);
        EXPECT_EQ(wave.loop_start_frame.value, 0U);
        EXPECT_EQ(wave.loop_length_frames.value, 0U);
    }
    for (const unsigned mode : {1U, 2U})
        expect_rejected_without_changes({{"loop_mode", mode}, {"loop_start_frame", 0}, {"loop_length_frames", 0}});
}

TEST_F(WaveDataParameterAlteration, RejectsOutOfBoundsAndOverflowingMergedWindowsWithoutMutation) {
    const std::vector<Json> invalid{{{"wave_start_frame", 5}, {"wave_length_frames", 16}},
                                    {{"wave_start_frame", 4294967295ULL}, {"wave_length_frames", 2}},
                                    {{"wave_length_frames", 4294967295ULL}},
                                    {{"loop_start_frame", 0}, {"loop_length_frames", 17}},
                                    {{"loop_start_frame", 16}, {"loop_length_frames", 1}},
                                    {{"loop_start_frame", 4294967295ULL}, {"loop_length_frames", 2}},
                                    {{"wave_start_frame", 4}, {"wave_length_frames", 12}},
                                    {{"loop_mode", 0}, {"loop_start_frame", 2}, {"loop_length_frames", 0}}};
    for (const auto &parameters : invalid) {
        SCOPED_TRACE(parameters.dump());
        expect_rejected_without_changes(parameters);
    }
    std::filesystem::copy_file(source, output);
    const auto original = image_bytes(source);
    const auto parsed = parse_wave_update(wave_update({{"root_key", 64}, {"loop_length_frames", 100}}));
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(axk::alter_hds(source, *parsed, output, {}, nullptr, true));
    EXPECT_EQ(image_bytes(source), original);
    EXPECT_EQ(image_bytes(output), original);
}

TEST_F(WaveDataParameterAlteration, PreservesCompletePcm8StorageDuringMetadataUpdate) {
    patch_wave(0x2aU, {std::byte{0}, std::byte{1}});
    const auto before = catalog(source);
    ASSERT_TRUE(before);
    const auto parsed = parse_wave_update(wave_update({{"root_key", 64}}));
    ASSERT_TRUE(parsed);

    const auto changed = axk::alter_hds(source, *parsed, output);

    ASSERT_TRUE(changed) << changed.error().message;
    const auto after = catalog(output);
    ASSERT_TRUE(after);
    const auto *old = wave_object(*before);
    const auto *current = wave_object(*after);
    ASSERT_NE(old, nullptr);
    ASSERT_NE(current, nullptr);
    auto expected = old->raw_payload;
    expected[0x7eU] = std::byte{64};
    axk::ByteWriter writer{expected};
    ASSERT_TRUE(writer.write_be16(0x80U, 0x1500U));
    EXPECT_EQ(current->raw_payload, expected);
    const auto &wave = std::get<axk::CurrentSmpl>(current->object.payload);
    EXPECT_EQ(wave.stored_sample_width_bytes.value, 1U);
    EXPECT_EQ(wave.pcm_transfer_control.value, 0x30U);
}

TEST_F(WaveDataParameterAlteration, RejectsUnsupportedTransferControlWithoutMutation) {
    patch_wave(0x84U, {std::byte{0x31}});

    expect_rejected_without_changes({{"root_key", 64}});
}

TEST_F(WaveDataParameterAlteration, RejectsIncompleteSegmentWithoutMutation) {
    patch_wave(0x1cU, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{44}});
    patch_wave(0x24U, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4}});
    const auto before = catalog(source);
    ASSERT_TRUE(before) << before.error().message;
    const auto *object = wave_object(*before);
    ASSERT_NE(object, nullptr);
    const auto *wave = std::get_if<axk::CurrentSmpl>(&object->object.payload);
    ASSERT_NE(wave, nullptr);
    ASSERT_EQ(wave->stored_segment_offset, 4U);
    ASSERT_EQ(wave->stored_segment_bytes, 40U);

    expect_rejected_without_changes({{"root_key", 64}});
}

} // namespace
