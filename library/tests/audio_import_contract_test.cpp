#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <tuple>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sndfile.h>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {
using Json = nlohmann::json;

Json fixture() {
    std::ifstream stream{std::filesystem::path{AXK_SOURCE_ROOT} /
                         "tests/fixtures/manifests/alteration/audio-import.json"};
    return Json::parse(stream);
}

bool write_audio(const std::filesystem::path &path, int channels, int subtype, int container) {
    SF_INFO info{};
    info.channels = channels;
    info.samplerate = 44'100;
    info.format = container | subtype;
    std::vector<double> samples(64U * static_cast<std::size_t>(channels));
    for (std::size_t index = 0; index < samples.size(); ++index)
        samples[index] = static_cast<double>(static_cast<int>(index % 17U) - 8) / 10.0;
    auto *file = sf_open(path.string().c_str(), SFM_WRITE, &info);
    if (!file)
        return false;
    const auto written = sf_writef_double(file, samples.data(), 64);
    return sf_close(file) == 0 && written == 64;
}

class AudioImportContract : public testing::TestWithParam<std::tuple<bool, bool, bool, int>> {
  protected:
    void SetUp() override {
        const auto [create_volume, bank, native, container] = GetParam();
        root_ = std::filesystem::temp_directory_path() /
                ("axklib-audio-import-contract-" + std::to_string(create_volume) + "-" + std::to_string(bank) + "-" +
                 std::to_string(native) + "-" + std::to_string(container));
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_ / "audio");
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    std::filesystem::path root_;
};

TEST_P(AudioImportContract, AppliesFrontendManifestAndPreservesSampleSettings) {
    const auto [create_volume, bank, native, container] = GetParam();
    auto document = fixture();
    auto &operations = document["operations"];
    for (auto &operation : operations) {
        if (operation["type"] == "insert_sbnk")
            operation["sample"]["storage_format"] = native ? "a3000_188" : "a4000_a5000_224";
        if (operation["type"] == "insert_sbac")
            operation["sample_bank"]["storage_format"] = native ? "a3000_188" : "a4000_a5000_224";
    }
    for (auto iterator = operations.begin(); iterator != operations.end();) {
        const auto type = iterator->at("type").get<std::string>();
        if ((!create_volume && type == "insert_volume") || (!bank && type == "insert_sbac"))
            iterator = operations.erase(iterator);
        else
            ++iterator;
    }
    ASSERT_TRUE(write_audio(root_ / "audio/import-0", 1, SF_FORMAT_PCM_16, container));
    ASSERT_TRUE(write_audio(root_ / "audio/import-1", 2, SF_FORMAT_PCM_24, container));
    axk::HdsBuildManifest source_spec{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = create_volume ? "Retained" : "Imported";
    source_spec.partitions.push_back({"Partition", {volume}});
    const auto source = root_ / "source.hds";
    const auto output = root_ / "output.hds";
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));
    const auto manifest = axk::parse_alteration_manifest(document.dump(), root_);
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    EXPECT_TRUE(catalog->issues.empty());
    EXPECT_EQ(catalog->objects.size(), bank ? 6U : 5U);
    const auto graph = axk::build_relationship_graph(*catalog);
    for (const auto &operation : operations) {
        if (operation.at("type") != "insert_sbnk")
            continue;
        const auto &sample = operation.at("sample");
        const auto found = std::ranges::find_if(catalog->objects, [&](const auto &object) {
            return object.object.header.type == axk::ObjectType::sbnk &&
                   object.object.header.name == sample.at("name").get<std::string>();
        });
        ASSERT_NE(found, catalog->objects.end());
        const auto *decoded = std::get_if<axk::CurrentSbnk>(&found->object.payload);
        ASSERT_NE(decoded, nullptr);
        EXPECT_EQ(decoded->storage.format,
                  native ? axk::SampleStorageFormat::a3000_188 : axk::SampleStorageFormat::a4000_a5000_224);
        EXPECT_EQ(decoded->raw_parameter_window.size(), native ? 188U : 224U);
        EXPECT_EQ(decoded->storage.header_revision, native ? 2U : 4U);
        const auto &parameters = sample.at("parameters");
        EXPECT_EQ(decoded->left.root_key, parameters.at("root_key").get<std::uint8_t>());
        EXPECT_EQ(decoded->left.fine_tune_cents, parameters.at("fine_tune_cents").get<std::int8_t>());
        EXPECT_EQ(decoded->key_range_low, parameters.at("key_low").get<std::uint8_t>());
        EXPECT_EQ(decoded->key_range_high, parameters.at("key_high").get<std::uint8_t>());
        EXPECT_EQ(decoded->velocity_range_low, parameters.at("velocity_low").get<std::uint8_t>());
        EXPECT_EQ(decoded->velocity_range_high, parameters.at("velocity_high").get<std::uint8_t>());
        EXPECT_EQ(decoded->sample_level, 100U);
        EXPECT_EQ(decoded->loop_mode, parameters.at("loop_mode").get<std::uint8_t>());
        EXPECT_EQ(decoded->left.wave_start_frame, 0U);
        EXPECT_EQ(decoded->left.wave_length_frames, 64U);
        EXPECT_EQ(decoded->left.wave_data_name, sample.at("waveform_name").get<std::string>());
        if (sample.contains("right_waveform_name")) {
            ASSERT_TRUE(decoded->right);
            EXPECT_EQ(decoded->right->wave_data_name, sample.at("right_waveform_name").get<std::string>());
            EXPECT_EQ(decoded->right->fine_tune_cents, decoded->left.fine_tune_cents);
            EXPECT_EQ(decoded->left.loop_start_frame, 8U);
            EXPECT_EQ(decoded->left.loop_length_frames, 32U);
            EXPECT_EQ(decoded->right->loop_start_frame, 8U);
            EXPECT_EQ(decoded->right->loop_length_frames, 32U);
        } else {
            EXPECT_FALSE(decoded->right);
        }
        const auto children = graph.children(found->key);
        EXPECT_EQ(children.size(), decoded->right ? 2U : 1U);
        for (const auto *link : children) {
            EXPECT_EQ(link->quality, axk::RelationshipQuality::known);
            EXPECT_TRUE(link->target_key);
        }
    }
    for (const auto &object : catalog->objects) {
        if (object.object.header.type == axk::ObjectType::smpl) {
            const auto wave = axk::decode_waveform(*reopened, object);
            ASSERT_TRUE(wave) << wave.error().message;
            EXPECT_EQ(wave->format.sample_width_bytes, 2U);
            EXPECT_EQ(wave->format.sample_rate, 44'100U);
        } else if (object.object.header.type == axk::ObjectType::sbac) {
            const auto &stored_bank = std::get<axk::CurrentSbac>(object.object.payload);
            EXPECT_EQ(stored_bank.parameter_tail_offset.has_value(), !native);
            EXPECT_EQ(object.raw_payload.size(), native ? 492U : 528U);
            const auto children = graph.children(object.key);
            EXPECT_EQ(children.size(), 2U);
            for (const auto *link : children) {
                EXPECT_EQ(link->quality, axk::RelationshipQuality::known);
                EXPECT_TRUE(link->target_key);
            }
        }
    }
    Json edit{
        {"id", "edit-import"}, {"partition_index", 0}, {"volume_name", "Imported"}, {"parameters", {{"level", 82}}}};
    edit["type"] = bank ? "update_sample_bank_parameters" : "update_sbnk_parameters";
    edit[bank ? "sample_bank_name" : "sample_name"] = bank ? "Imported Bank" : "Mono";
    if (!bank) {
        const auto sample_operation = std::ranges::find_if(
            operations, [](const auto &operation) { return operation.at("type") == "insert_sbnk"; });
        edit["sample_name"] = sample_operation->at("sample").at("name");
    } else {
        const auto bank_operation = std::ranges::find_if(
            operations, [](const auto &operation) { return operation.at("type") == "insert_sbac"; });
        edit["sample_bank_name"] = bank_operation->at("sample_bank").at("name");
    }
    const auto edit_manifest =
        axk::parse_alteration_manifest(Json{{"schema_version", "1.0"}, {"operations", Json::array({edit})}}.dump());
    ASSERT_TRUE(edit_manifest);
    const auto edited_path = root_ / "edited.hds";
    const auto edited = axk::alter_hds(output, *edit_manifest, edited_path);
    ASSERT_TRUE(edited) << edited.error().message;
    const auto edited_image = axk::open_image(edited_path);
    ASSERT_TRUE(edited_image);
    const auto edited_catalog = axk::build_object_catalog(*edited_image);
    ASSERT_TRUE(edited_catalog);
    for (const auto &object : edited_catalog->objects) {
        if (const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload)) {
            EXPECT_EQ(sample->storage.format,
                      native ? axk::SampleStorageFormat::a3000_188 : axk::SampleStorageFormat::a4000_a5000_224);
            if (bank || object.object.header.name == edit.at("sample_name").get<std::string>())
                EXPECT_EQ(sample->sample_level, 82U);
        } else if (object.object.header.type == axk::ObjectType::smpl) {
            const auto original = std::ranges::find_if(catalog->objects, [&](const auto &candidate) {
                return candidate.object.header.type == axk::ObjectType::smpl &&
                       candidate.object.header.name == object.object.header.name;
            });
            ASSERT_NE(original, catalog->objects.end());
            EXPECT_EQ(object.raw_payload, original->raw_payload);
        } else if (const auto *stored_bank = std::get_if<axk::CurrentSbac>(&object.object.payload)) {
            EXPECT_EQ(stored_bank->parameter_tail_offset.has_value(), !native);
            EXPECT_EQ(object.raw_payload.size(), native ? 492U : 528U);
        }
    }
}

// Keep libsndfile's anonymous enum out of Combine's tuple (MSVC 14.51 C2995).
constexpr std::array<int, 3> audio_containers{SF_FORMAT_WAV, SF_FORMAT_AIFF, SF_FORMAT_FLAC};

INSTANTIATE_TEST_SUITE_P(VolumeAndGrouping, AudioImportContract,
                         testing::Combine(testing::Bool(), testing::Bool(), testing::Bool(),
                                          testing::ValuesIn(audio_containers)));

TEST(AudioImportManifest, RejectsObsoleteFlatSampleParameters) {
    auto document = fixture();
    auto &sample = document["operations"][2]["sample"];
    sample["fine_tune_cents"] = sample["parameters"]["fine_tune_cents"];
    const auto parsed = axk::parse_alteration_manifest(document.dump());
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().message.find("sample has unknown field fine_tune_cents"), std::string::npos);
}
} // namespace
