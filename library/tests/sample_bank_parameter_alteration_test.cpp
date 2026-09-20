#include <algorithm>
#include <cstddef>
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
#include "axklib/catalog.hpp"
#include "axklib/writer.hpp"

namespace {
using Json = nlohmann::json;

Json bank_update(Json parameters = {{"level", 87}}) {
    return {{"id", "bank-parameters"},    {"type", "update_sample_bank_parameters"},
            {"partition_index", 0},       {"volume_name", "Samples"},
            {"sample_bank_name", "Bank"}, {"parameters", std::move(parameters)}};
}

axk::Result<axk::AlterationManifest> parse_bank_update(const Json &operation) {
    return axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
}

std::vector<char> read_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

class SampleBankParameterAlteration : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-bank-parameters-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
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
        wave.frame_count = 4U;
        wave.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
        ASSERT_TRUE(axk::write_wav_atomic(audio, wave));
        axk::VolumeSpec volume;
        volume.name = "Samples";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        axk::SampleSpec member;
        member.name = "Member A";
        member.waveform_id = "wave";
        member.parameters.level = 65U;
        member.parameters.key_high = 100U;
        volume.samples.push_back(member);
        member.name = "Member B";
        member.parameters.level = 75U;
        member.parameters.key_high = 50U;
        volume.samples.push_back(member);
        member.name = "Direct";
        member.parameters.level = 95U;
        volume.samples.push_back(member);
        volume.sample_banks.push_back({"Bank", {"Member A", "Member B"}});
        volume.programs.push_back({1U, "Direct", {{"SBNK", "Direct", {.receive = axk::ProgramReceiveInherit{}}}}});
        const axk::HdsBuildManifest build{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(build, source);
        ASSERT_TRUE(written) << written.error().message;
        patch_bank(0x43U, '\x5a');
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void patch_bank(std::size_t offset, char value) {
        const auto image = read_bytes(source);
        constexpr std::string_view magic{"FSFSDEV3SPLXSBAC"};
        const auto found = std::search(image.begin(), image.end(), magic.begin(), magic.end());
        ASSERT_NE(found, image.end());
        ASSERT_EQ(
            std::search(found + static_cast<std::ptrdiff_t>(magic.size()), image.end(), magic.begin(), magic.end()),
            image.end());
        std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(file);
        file.seekp(static_cast<std::streamoff>(found - image.begin()) + static_cast<std::streamoff>(offset));
        file.put(value);
        ASSERT_TRUE(file);
    }
};

TEST(SampleBankParameterManifest, AcceptsSparseValuesAndRejectsEmptyUnknownAndInvalidParameters) {
    const auto valid = parse_bank_update(bank_update());
    ASSERT_TRUE(valid) << valid.error().message;
    EXPECT_EQ(axk::operation_type_name(valid->operations.front().data), "update_sample_bank_parameters");
    for (const auto &parameters :
         {Json::object(), Json{{"feg", Json::object()}}, Json{{"unknown", 1}}, Json{{"level", 128}},
          Json{{"level", -1}}, Json{{"level", true}}, Json{{"level", 1.5}}, Json{{"sample_flags", 1}}, Json(nullptr)}) {
        SCOPED_TRACE(parameters.dump());
        EXPECT_FALSE(parse_bank_update(bank_update(parameters)));
    }
    auto missing = bank_update();
    missing.erase("parameters");
    EXPECT_FALSE(parse_bank_update(missing));
    missing = bank_update();
    missing["sample_bank_name"] = "";
    EXPECT_FALSE(parse_bank_update(missing));
}

TEST_F(SampleBankParameterAlteration, ChangesBankAndEveryMemberLevelWhilePreservingAllOtherPayloadBytes) {
    const auto parsed = parse_bank_update(bank_update());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_bytes(source);
    const auto source_image = axk::open_image(source);
    ASSERT_TRUE(source_image);
    const auto before = axk::build_object_catalog(*source_image);
    ASSERT_TRUE(before);

    const auto applied = axk::alter_hds(source, *parsed, output);

    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(read_bytes(source), original);
    const auto output_image = axk::open_image(output);
    ASSERT_TRUE(output_image);
    const auto after = axk::build_object_catalog(*output_image);
    ASSERT_TRUE(after);
    ASSERT_EQ(before->objects.size(), after->objects.size());
    for (const auto &old : before->objects) {
        const auto current = std::ranges::find(after->objects, old.key, &axk::ObjectSnapshot::key);
        ASSERT_NE(current, after->objects.end());
        auto expected = old.raw_payload;
        if (old.object.header.type == axk::ObjectType::sbac) {
            expected[0xe6U] = std::byte{87};
            const auto *bank = std::get_if<axk::CurrentSbac>(&current->object.payload);
            ASSERT_NE(bank, nullptr);
            EXPECT_TRUE(bank->pending_parameter_numbers.empty());
            EXPECT_TRUE(bank->reserved_pending_parameter_numbers.empty());
        } else if (old.object.header.type == axk::ObjectType::sbnk && old.object.header.name != "Direct") {
            expected[0x116U] = std::byte{87};
        }
        EXPECT_EQ(current->raw_payload, expected) << old.object.header.name;
    }
    const auto again = root / "again.hds";
    ASSERT_TRUE(axk::alter_hds(output, *parsed, again));
    EXPECT_EQ(read_bytes(output), read_bytes(again));
}

TEST_F(SampleBankParameterAlteration,
       UpdatesRootKeyAndRecalculatesBankAndMemberEqWithoutChangingPcmOrUnrelatedObjects) {
    const auto parsed = parse_bank_update(
        bank_update({{"root_key", 64}, {"sample_eq_type", 1}, {"sample_eq_frequency", 30}, {"sample_eq_gain_db", -6}}));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto source_image = axk::open_image(source);
    ASSERT_TRUE(source_image);
    const auto before = axk::build_object_catalog(*source_image);
    ASSERT_TRUE(before);

    const auto applied = axk::alter_hds(source, *parsed, output);

    ASSERT_TRUE(applied) << applied.error().message;
    const auto output_image = axk::open_image(output);
    ASSERT_TRUE(output_image);
    const auto after = axk::build_object_catalog(*output_image);
    ASSERT_TRUE(after);
    for (const auto &old : before->objects) {
        const auto current = std::ranges::find(after->objects, old.key, &axk::ObjectSnapshot::key);
        ASSERT_NE(current, after->objects.end());
        if (old.object.header.type == axk::ObjectType::sbac) {
            const auto *bank = std::get_if<axk::CurrentSbac>(&current->object.payload);
            ASSERT_NE(bank, nullptr);
            EXPECT_EQ(bank->raw_sample_parameter_block[0x2eU], std::byte{64});
            EXPECT_FALSE(std::ranges::equal(std::span{old.raw_payload}.subspan(0x122U, 10U),
                                            std::span{current->raw_payload}.subspan(0x122U, 10U)));
            EXPECT_EQ(current->raw_payload[0x43U], std::byte{0x5a});
        } else if (old.object.header.type == axk::ObjectType::sbnk && old.object.header.name != "Direct") {
            const auto *sample = std::get_if<axk::CurrentSbnk>(&current->object.payload);
            ASSERT_NE(sample, nullptr);
            EXPECT_EQ(sample->left.root_key, 64U);
            EXPECT_FALSE(std::ranges::equal(std::span{old.raw_payload}.subspan(0x152U, 10U),
                                            std::span{current->raw_payload}.subspan(0x152U, 10U)));
        } else {
            EXPECT_EQ(current->raw_payload, old.raw_payload) << old.object.header.name;
        }
    }
}

TEST_F(SampleBankParameterAlteration, InvalidMergedRangeInSecondMemberPreservesSourceAndExistingDestination) {
    const auto parsed = parse_bank_update(bank_update({{"key_low", 60}}));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_bytes(source);
    std::filesystem::copy_file(source, output);
    const auto destination = read_bytes(output);

    EXPECT_FALSE(axk::alter_hds(source, *parsed, output, {}, nullptr, true));

    EXPECT_EQ(read_bytes(source), original);
    EXPECT_EQ(read_bytes(output), destination);
    const auto absent = root / "absent.hds";
    EXPECT_FALSE(axk::alter_hds(source, *parsed, absent));
    EXPECT_FALSE(std::filesystem::exists(absent));
}

TEST_F(SampleBankParameterAlteration, PartialOriginalKeyRangeUsesBankAndMemberStateWithinBatch) {
    auto first = bank_update({{"root_key", 40}});
    first["id"] = "root";
    const auto original = read_bytes(source);
    for (const auto high : {50, 35}) {
        const auto parsed = axk::parse_alteration_manifest(
            Json{{"schema_version", "1.0"},
                 {"operations", Json::array({first, bank_update({{"key_low", 255}, {"key_high", high}})})}}
                .dump());
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto destination = root / (std::to_string(high) + ".hds");
        const auto applied = axk::alter_hds(source, *parsed, destination);
        EXPECT_EQ(read_bytes(source), original);
        if (high == 35) {
            EXPECT_FALSE(applied);
            EXPECT_FALSE(std::filesystem::exists(destination));
            continue;
        }
        ASSERT_TRUE(applied) << applied.error().message;
        const auto image = axk::open_image(destination);
        ASSERT_TRUE(image);
        const auto catalog = axk::build_object_catalog(*image);
        ASSERT_TRUE(catalog);
        for (const auto &object : catalog->objects) {
            const bool bank = object.object.header.type == axk::ObjectType::sbac;
            const bool member =
                object.object.header.type == axk::ObjectType::sbnk && object.object.header.name != "Direct";
            if (bank || member) {
                const auto base = bank ? 0x78U : 0xa8U;
                EXPECT_EQ(object.raw_payload[base + 0x2eU], std::byte{40});
                EXPECT_EQ(object.raw_payload[base + 0x3bU], std::byte{255});
                EXPECT_EQ(object.raw_payload[base + 0x3aU], std::byte{50});
            }
        }
    }
}

TEST_F(SampleBankParameterAlteration, RejectsPendingPropagationWithoutDiscardingStateOrPublishingChanges) {
    patch_bank(0x137U, '\x01');
    const auto parsed = parse_bank_update(bank_update());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_bytes(source);
    const auto image = axk::open_image(source);
    ASSERT_TRUE(image);
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    bool pending_seen = false;
    for (const auto &object : catalog->objects) {
        if (const auto *bank = std::get_if<axk::CurrentSbac>(&object.object.payload))
            pending_seen = !bank->pending_parameter_numbers.empty();
    }
    ASSERT_TRUE(pending_seen);

    EXPECT_FALSE(axk::alter_hds(source, *parsed, output));

    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read_bytes(source), original);
}

TEST_F(SampleBankParameterAlteration, MixedFormatsValidateInsertionAndUpdatesWithoutPartialPublication) {
    for (const bool native_bank : {false, true}) {
        for (const bool insert_bank : {false, true}) {
            const auto bank_format =
                native_bank ? axk::SampleStorageFormat::a3000_188 : axk::SampleStorageFormat::a4000_a5000_224;
            const auto other_format =
                native_bank ? axk::SampleStorageFormat::a4000_a5000_224 : axk::SampleStorageFormat::a3000_188;
            axk::VolumeSpec volume;
            volume.name = "Samples";
            volume.waveforms.push_back({"wave", "Wave", root / "tone.wav", 60U, {}});
            axk::SampleSpec member;
            member.name = "Member A";
            member.waveform_id = "wave";
            member.storage_format = bank_format;
            volume.samples.push_back(member);
            member.name = "Member B";
            member.storage_format = other_format;
            volume.samples.push_back(member);
            if (!insert_bank) {
                axk::SampleBankSpec bank{"Bank", {"Member A", "Member B"}};
                bank.storage_format = bank_format;
                volume.sample_banks.push_back(bank);
            }
            const auto suffix = std::to_string(native_bank) + std::to_string(insert_bank);
            const auto mixed = root / ("mixed" + suffix + ".hds");
            ASSERT_TRUE(axk::write_hds_image({"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}}, mixed));
            const auto original = read_bytes(mixed);
            const auto existing = root / ("existing" + suffix + ".hds");
            std::filesystem::copy_file(mixed, existing);
            for (const bool compatible : {false, true}) {
                const Json parameters = compatible    ? Json{{"level", 82}}
                                        : native_bank ? Json{{"coarse_tune", 100}}
                                                      : Json{{"portamento_time", 90}};
                auto operation = bank_update(parameters);
                if (insert_bank) {
                    operation = {{"id", "bank"},
                                 {"type", "insert_sbac"},
                                 {"partition_index", 0},
                                 {"volume_name", "Samples"},
                                 {"sample_bank",
                                  {{"name", "Bank"},
                                   {"member_samples", {"Member A", "Member B"}},
                                   {"storage_format", axk::sample_storage_format_name(bank_format)},
                                   {"parameter_overrides", parameters}}}};
                }
                const auto parsed = parse_bank_update(operation);
                ASSERT_TRUE(parsed) << parsed.error().message;
                const auto destination = root / ("result" + suffix + std::to_string(compatible) + ".hds");
                const auto result = axk::alter_hds(mixed, *parsed, destination);
                EXPECT_EQ(read_bytes(mixed), original);
                if (!compatible) {
                    ASSERT_FALSE(result);
                    EXPECT_FALSE(std::filesystem::exists(destination));
                    EXPECT_FALSE(axk::alter_hds(mixed, *parsed, existing, {}, nullptr, true));
                    EXPECT_EQ(read_bytes(existing), original);
                    continue;
                }
                ASSERT_TRUE(result) << result.error().message;
                const auto image = axk::open_image(destination);
                ASSERT_TRUE(image);
                const auto catalog = axk::build_object_catalog(*image);
                ASSERT_TRUE(catalog);
                for (const auto &object : catalog->objects) {
                    if (const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload)) {
                        EXPECT_EQ(sample->storage.format,
                                  object.object.header.name == "Member A" ? bank_format : other_format);
                        EXPECT_EQ(sample->sample_level, 82U);
                    } else if (const auto *bank = std::get_if<axk::CurrentSbac>(&object.object.payload)) {
                        EXPECT_EQ(bank->parameter_tail_offset.has_value(), !native_bank);
                        EXPECT_EQ(bank->raw_sample_parameter_block[0x6e], std::byte{82});
                    }
                }
            }
        }
    }
}

} // namespace
