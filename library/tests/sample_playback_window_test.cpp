#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
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

Json window_sample() {
    return {{"name", "Window"},
            {"waveform_id", "left"},
            {"parameters", {{"loop_mode", 0}}},
            {"playback_window", {{"start_frame", 65536}, {"length_frames", 8}}}};
}

Json build_manifest(const Json &samples) {
    return {{"schema_version", "1.0"},
            {"size_bytes", 4U * 1024U * 1024U},
            {"partitions", Json::array({{{"name", "Partition"},
                                         {"volumes", Json::array({{{"name", "Windows"},
                                                                   {"waveforms", Json::array({{{"id", "left"},
                                                                                               {"name", "Left"},
                                                                                               {"path", "left.wav"},
                                                                                               {"root_key", 60},
                                                                                               {"loop_mode", 0}},
                                                                                              {{"id", "right"},
                                                                                               {"name", "Right"},
                                                                                               {"path", "right.wav"},
                                                                                               {"root_key", 60},
                                                                                               {"loop_mode", 0}}})},
                                                                   {"samples", samples}}})}}})}};
}

class SamplePlaybackWindow : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path output;
    std::vector<std::byte> left_pcm;
    std::vector<std::byte> right_pcm;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-sample-window-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
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
        output = root / "window.hds";
        axk::Waveform wave;
        wave.format = {1U, 2U, 44'100U};
        wave.frame_count = 65544U;
        for (std::uint32_t frame = 0; frame < 65544U; ++frame) {
            left_pcm.push_back(static_cast<std::byte>(frame & 0xffU));
            left_pcm.push_back(static_cast<std::byte>((frame >> 8U) & 0x7fU));
            right_pcm.push_back(static_cast<std::byte>((frame + 1U) & 0xffU));
            right_pcm.push_back(static_cast<std::byte>(0x80U | ((frame >> 8U) & 0x7fU)));
        }
        wave.pcm = left_pcm;
        ASSERT_TRUE(axk::write_wav_atomic(root / "left.wav", wave));
        wave.pcm = right_pcm;
        ASSERT_TRUE(axk::write_wav_atomic(root / "right.wav", wave));
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void expect_physical_pcm_unchanged(const axk::Container &image, const axk::ObjectCatalog &catalog) {
        std::size_t count{};
        for (const auto &object : catalog.objects) {
            if (object.object.header.type != axk::ObjectType::smpl)
                continue;
            ++count;
            const auto *metadata = std::get_if<axk::CurrentSmpl>(&object.object.payload);
            ASSERT_NE(metadata, nullptr);
            EXPECT_EQ(metadata->wave_start_frame.value, 0U);
            EXPECT_EQ(metadata->wave_length_frames.value, 65544U);
            const auto waveform = axk::decode_waveform(image, object);
            ASSERT_TRUE(waveform) << waveform.error().message;
            auto expected = object.object.header.name == "Left" ? left_pcm : right_pcm;
            const auto &original = object.object.header.name == "Left" ? left_pcm : right_pcm;
            expected.insert(expected.end(), original.begin(), original.begin() + 8);
            EXPECT_EQ(waveform->pcm, expected);
        }
        EXPECT_EQ(count, 2U);
    }
};

TEST(SamplePlaybackWindowManifest, RequiresCompleteStrictWindowOutsideSampleParameters) {
    EXPECT_TRUE(axk::parse_hds_build_manifest(build_manifest(Json::array({window_sample()})).dump()));
    const std::vector<Json> invalid_windows{Json::object(),
                                            Json(nullptr),
                                            {{"start_frame", 1}},
                                            {{"length_frames", 8}},
                                            {{"start_frame", -1}, {"length_frames", 8}},
                                            {{"start_frame", true}, {"length_frames", 8}},
                                            {{"start_frame", 1.5}, {"length_frames", 8}},
                                            {{"start_frame", 0}, {"length_frames", 4294967296ULL}},
                                            {{"start_frame", 0}, {"length_frames", 8}, {"unknown", 1}}};
    for (const auto &window : invalid_windows) {
        SCOPED_TRACE(window.dump());
        auto sample = window_sample();
        sample["playback_window"] = window;
        EXPECT_FALSE(axk::parse_hds_build_manifest(build_manifest(Json::array({sample})).dump()));
    }
    auto misplaced = window_sample();
    misplaced["parameters"]["playback_window"] = misplaced["playback_window"];
    misplaced.erase("playback_window");
    EXPECT_FALSE(axk::parse_hds_build_manifest(build_manifest(Json::array({misplaced})).dump()));
}

TEST_F(SamplePlaybackWindow, MonoWindowAbove16BitRangeKeepsPcmAndDefaultsLoopToTheLogicalWindow) {
    const auto manifest = axk::parse_hds_build_manifest(build_manifest(Json::array({window_sample()})).dump(), root);
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto written = axk::write_hds_image(*manifest, output);
    ASSERT_TRUE(written) << written.error().message;
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    std::size_t samples{};
    for (const auto &object : catalog->objects) {
        const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload);
        if (!sample)
            continue;
        ++samples;
        EXPECT_EQ(sample->left.wave_start_frame, 65536U);
        EXPECT_EQ(sample->left.wave_length_frames, 8U);
        EXPECT_EQ(sample->left.loop_start_frame, 65536U);
        EXPECT_EQ(sample->left.loop_length_frames, 8U);
        EXPECT_FALSE(sample->right_slot_present);
        EXPECT_FALSE(sample->right);
        EXPECT_TRUE(sample->inactive_right.wave_data_name.empty());
        EXPECT_EQ(sample->inactive_right.wave_start_frame, 65536U);
        EXPECT_EQ(sample->inactive_right.wave_length_frames, 8U);
        EXPECT_EQ(sample->inactive_right.loop_start_frame, 65536U);
        EXPECT_EQ(sample->inactive_right.loop_length_frames, 8U);
        const axk::ByteReader reader{object.raw_payload};
        EXPECT_EQ(reader.be32(0x15cU).value(), 65544U);
        EXPECT_EQ(reader.be32(0x160U).value(), 65544U);
    }
    EXPECT_EQ(samples, 1U);
    expect_physical_pcm_unchanged(*image, *catalog);
}

TEST_F(SamplePlaybackWindow, StereoWindowAndAbsoluteLoopAreMirroredIntoBothActiveMembers) {
    auto spec = window_sample();
    spec["right_waveform_id"] = "right";
    spec["parameters"] = {{"loop_mode", 1}, {"loop_start_frame", 65538}, {"loop_length_frames", 6}};
    const auto manifest = axk::parse_hds_build_manifest(build_manifest(Json::array({spec})).dump(), root);
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto written = axk::write_hds_image(*manifest, output);
    ASSERT_TRUE(written) << written.error().message;
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    std::size_t samples{};
    for (const auto &object : catalog->objects) {
        const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload);
        if (!sample)
            continue;
        ++samples;
        ASSERT_TRUE(sample->right_slot_present);
        ASSERT_TRUE(sample->right);
        EXPECT_EQ(sample->left.wave_data_name, "Left");
        EXPECT_EQ(sample->right->wave_data_name, "Right");
        for (const auto *member : {&sample->left, &*sample->right}) {
            EXPECT_EQ(member->wave_start_frame, 65536U);
            EXPECT_EQ(member->wave_length_frames, 8U);
            EXPECT_EQ(member->loop_start_frame, 65538U);
            EXPECT_EQ(member->loop_length_frames, 6U);
        }
        const axk::ByteReader reader{object.raw_payload};
        EXPECT_EQ(reader.be32(0x15cU).value(), 65544U);
        EXPECT_EQ(reader.be32(0x160U).value(), 65544U);
    }
    EXPECT_EQ(samples, 1U);
    expect_physical_pcm_unchanged(*image, *catalog);
}

TEST_F(SamplePlaybackWindow, AllSixModesRetainAbsoluteLoopCoordinatesWithinTheirPlaybackWindow) {
    auto samples = Json::array();
    for (unsigned mode = 0; mode < 6U; ++mode) {
        auto spec = window_sample();
        spec["name"] = "Mode" + std::to_string(mode);
        spec["parameters"] = {{"loop_mode", mode}, {"loop_start_frame", 65538}, {"loop_length_frames", 6}};
        samples.push_back(spec);
    }
    const auto manifest = axk::parse_hds_build_manifest(build_manifest(samples).dump(), root);
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto written = axk::write_hds_image(*manifest, output);
    ASSERT_TRUE(written) << written.error().message;
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    std::size_t count{};
    for (const auto &object : catalog->objects) {
        const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload);
        if (!sample)
            continue;
        ++count;
        EXPECT_EQ(sample->sample_name, "Mode" + std::to_string(sample->loop_mode));
        EXPECT_EQ(sample->left.wave_start_frame, 65536U);
        EXPECT_EQ(sample->left.loop_start_frame, 65538U);
        EXPECT_EQ(sample->left.loop_length_frames, 6U);
    }
    EXPECT_EQ(count, 6U);
}

TEST_F(SamplePlaybackWindow, RejectsWindowsOutsideOriginalPcmAndInvalidAbsoluteLoopsWithoutPublication) {
    std::vector<Json> invalid_samples;
    for (const auto &window : std::vector<Json>{{{"start_frame", 65544}, {"length_frames", 1}},
                                                {{"start_frame", 65536}, {"length_frames", 9}},
                                                {{"start_frame", 4294967295ULL}, {"length_frames", 2}},
                                                {{"start_frame", 0}, {"length_frames", 0}}}) {
        auto spec = window_sample();
        spec["playback_window"] = window;
        invalid_samples.push_back(spec);
    }
    for (const auto &parameters :
         std::vector<Json>{{{"loop_mode", 1}},
                           {{"loop_mode", 2}},
                           {{"loop_mode", 0}, {"loop_start_frame", 65535}, {"loop_length_frames", 1}},
                           {{"loop_mode", 1}, {"loop_start_frame", 65538}, {"loop_length_frames", 7}},
                           {{"loop_mode", 1}, {"loop_start_frame", 65544}, {"loop_length_frames", 1}}}) {
        auto spec = window_sample();
        spec["parameters"] = parameters;
        invalid_samples.push_back(spec);
    }
    for (const auto &spec : invalid_samples) {
        SCOPED_TRACE(spec.dump());
        const auto manifest = axk::parse_hds_build_manifest(build_manifest(Json::array({spec})).dump(), root);
        if (manifest) {
            EXPECT_FALSE(axk::write_hds_image(*manifest, output));
        }
        EXPECT_FALSE(std::filesystem::exists(output));
    }
}

TEST_F(SamplePlaybackWindow, InsertionUsesQueuedWaveDataWindowAndExplicitWindowsUsePhysicalStoredBounds) {
    const Json existing{{"name", "Existing"}, {"waveform_id", "left"}, {"parameters", {{"loop_mode", 0}}}};
    const auto build = axk::parse_hds_build_manifest(build_manifest(Json::array({existing})).dump(), root);
    ASSERT_TRUE(build) << build.error().message;
    const auto source = root / "source.hds";
    ASSERT_TRUE(axk::write_hds_image(*build, source));
    const auto old_image = axk::open_image(source);
    ASSERT_TRUE(old_image);
    const auto before = axk::build_object_catalog(*old_image);
    ASSERT_TRUE(before);
    const Json operations = Json::array(
        {{{"id", "window"},
          {"type", "update_wave_data_parameters"},
          {"partition_index", 0},
          {"volume_name", "Windows"},
          {"waveform_name", "Left"},
          {"parameters",
           {{"loop_mode", 0},
            {"wave_start_frame", 65536},
            {"wave_length_frames", 8},
            {"loop_start_frame", 0},
            {"loop_length_frames", 0}}}},
         {{"id", "inherited"},
          {"type", "insert_sbnk"},
          {"partition_index", 0},
          {"volume_name", "Windows"},
          {"sample", {{"name", "Inherited"}, {"waveform_name", "Left"}, {"parameters", {{"loop_mode", 0}}}}}},
         {{"id", "explicit"},
          {"type", "insert_sbnk"},
          {"partition_index", 0},
          {"volume_name", "Windows"},
          {"sample",
           {{"name", "Explicit"},
            {"waveform_name", "Left"},
            {"parameters", {{"loop_mode", 0}}},
            {"playback_window", {{"start_frame", 1}, {"length_frames", 16}}}}}}});
    const auto parsed =
        axk::parse_alteration_manifest(Json{{"schema_version", "1.0"}, {"operations", operations}}.dump());
    ASSERT_TRUE(parsed) << parsed.error().message;

    const auto altered = axk::alter_hds(source, *parsed, output);

    ASSERT_TRUE(altered) << altered.error().message;
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    const auto after = axk::build_object_catalog(*image);
    ASSERT_TRUE(after);
    std::size_t inserted{};
    for (const auto &object : after->objects) {
        const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload);
        if (!sample || sample->sample_name == "Existing")
            continue;
        ++inserted;
        const bool inherited = sample->sample_name == "Inherited";
        ASSERT_TRUE(inherited || sample->sample_name == "Explicit");
        EXPECT_EQ(sample->left.wave_start_frame, inherited ? 65536U : 1U);
        EXPECT_EQ(sample->left.wave_length_frames, inherited ? 8U : 16U);
        EXPECT_EQ(sample->left.loop_start_frame, inherited ? 65536U : 1U);
        EXPECT_EQ(sample->left.loop_length_frames, inherited ? 8U : 16U);
    }
    EXPECT_EQ(inserted, 2U);
    for (const auto &old : before->objects) {
        const auto current = std::ranges::find(after->objects, old.key, &axk::ObjectSnapshot::key);
        ASSERT_NE(current, after->objects.end());
        if (old.object.header.type != axk::ObjectType::smpl || old.object.header.name != "Left")
            EXPECT_EQ(current->raw_payload, old.raw_payload) << old.object.header.name;
        else
            EXPECT_TRUE(std::ranges::equal(std::span{current->raw_payload}.subspan(512U),
                                           std::span{old.raw_payload}.subspan(512U)));
    }
}

TEST_F(SamplePlaybackWindow, InsertionRejectsWaveDataWithUnsupportedTransferControlWithoutPublication) {
    const Json existing{{"name", "Existing"}, {"waveform_id", "left"}, {"parameters", {{"loop_mode", 0}}}};
    const auto build = axk::parse_hds_build_manifest(build_manifest(Json::array({existing})).dump(), root);
    ASSERT_TRUE(build);
    const auto source = root / "source.hds";
    ASSERT_TRUE(axk::write_hds_image(*build, source));
    std::vector<std::byte> payload;
    {
        const auto image = axk::open_image(source);
        ASSERT_TRUE(image);
        const auto catalog = axk::build_object_catalog(*image);
        ASSERT_TRUE(catalog);
        const auto found = std::ranges::find_if(catalog->objects, [](const auto &object) {
            return object.object.header.type == axk::ObjectType::smpl && object.object.header.name == "Left";
        });
        ASSERT_NE(found, catalog->objects.end());
        payload = found->raw_payload;
    }
    const auto read_bytes = [](const std::filesystem::path &path) {
        std::ifstream input{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    };
    const auto bytes = read_bytes(source);
    const auto found =
        std::search(bytes.begin(), bytes.end(), payload.begin(), payload.end(), [](char left, std::byte right) {
            return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
        });
    ASSERT_NE(found, bytes.end());
    {
        std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(file);
        file.seekp(static_cast<std::streamoff>(found - bytes.begin()) + 0x84);
        file.put('\x31');
        ASSERT_TRUE(file);
    }
    const auto original = read_bytes(source);
    axk::SampleSpec sample;
    sample.name = "Invalid";
    sample.waveform_id = "Left";
    sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward;
    const axk::AlterationManifest manifest{
        "1.0", {{"insert", axk::InsertSampleOperation{axk::PartitionIndex{0}, "Windows", sample}}}};

    const auto altered = axk::alter_hds(source, manifest, output);

    EXPECT_FALSE(altered);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read_bytes(source), original);
}

} // namespace
