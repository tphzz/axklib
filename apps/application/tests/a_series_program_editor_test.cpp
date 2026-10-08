#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a_series_program_context.hpp"
#include "a_series_program_editor.hpp"
#include "axklib/application/program_editor.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/effects.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

using Json = nlohmann::json;

axk::ObjectSnapshot editor_snapshot(std::uint32_t revision) {
    const auto prepared = axk::detail::prepare_prog_payload(axk::ProgramSpec{33U, "Editor", {}});
    EXPECT_TRUE(prepared);
    if (!prepared)
        return {};
    auto bytes = *prepared;
    if (revision != 4U)
        bytes.resize(bytes.size() - 0xb0U);
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_be32(0x14U, revision));
    EXPECT_TRUE(writer.write_be32(0x18U, static_cast<std::uint32_t>(bytes.size() - (revision == 4U ? 0xe0U : 0x30U))));
    EXPECT_TRUE(writer.write_be32(0x1cU, revision == 4U ? static_cast<std::uint32_t>(bytes.size() - 0x30U) : 0U));
    EXPECT_TRUE(writer.write_be16(0x96U, 3U));
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto row = 0x120U + index * 0x38U;
        EXPECT_TRUE(writer.write_ascii_field(row, 16U, index < 2U ? "Duplicate" : index == 2U ? "Bank" : "Dormant"));
        EXPECT_TRUE(writer.write_u8(row + 0x14U, index == 2U ? 0x11U : 0x10U));
        EXPECT_TRUE(writer.write_u8(row + 0x16U, static_cast<std::uint8_t>(11U + index)));
    }
    for (std::size_t slot = 0; slot < (revision == 4U ? 6U : 3U); ++slot) {
        const auto offset = slot < 3U ? 0x98U + slot * 0x28U : bytes.size() - 0xb0U + (slot - 3U) * 0x28U;
        for (std::size_t word = 0; word < 16U; ++word)
            EXPECT_TRUE(
                writer.write_be16(offset + 8U + word * 2U, static_cast<std::uint16_t>(0x8100U + slot * 16U + word)));
    }
    const auto decoded = axk::decode_object(bytes);
    EXPECT_TRUE(decoded);
    if (!decoded)
        return {};
    const axk::ObjectPlacement placement{
        axk::PartitionIndex{2U}, "Partition", axk::SfsId{1U}, "Programs", "PROG", "033", ""};
    return {"program",
            axk::PartitionIndex{2U},
            axk::SfsId{17U},
            "partition:2",
            *decoded,
            placement,
            std::move(bytes),
            {},
            axk::PlacementResolution::exact};
}

void redecode(axk::ObjectSnapshot &snapshot) {
    const auto decoded = axk::decode_object(snapshot.raw_payload);
    ASSERT_TRUE(decoded);
    snapshot.object = *decoded;
}

TEST(ProgramEditorSnapshot, NativeAndCurrentPreservePlacementNumericIdentityAndDigest) {
    for (const auto revision : {2U, 4U}) {
        SCOPED_TRACE(revision);
        const auto snapshot = editor_snapshot(revision);
        const auto result = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, true);
        ASSERT_TRUE(result.is_object());
        EXPECT_EQ(result.at("profile"), "a-series/program");
        EXPECT_EQ(result.at("editable"), true);
        EXPECT_EQ(result.at("reason"), "");
        EXPECT_EQ(result.at("partitionIndex"), 2U);
        EXPECT_EQ(result.at("volumeName"), "Programs");
        EXPECT_EQ(result.at("programNumber"), 33U);
        EXPECT_EQ(result.at("programName"), "Editor");
        EXPECT_EQ(result.at("storageRevision"), revision);
        EXPECT_EQ(result.at("model"), revision == 2U ? "A3000" : "A5000");
        EXPECT_EQ(result.at("payloadSha256"),
                  axk::package_internal::hex_digest(axk::package_internal::sha256(snapshot.raw_payload)));
        ASSERT_TRUE(result.at("values").is_object());
        for (const auto &value : result.at("values"))
            EXPECT_TRUE(value.is_number() || value.is_boolean());
    }
}

TEST(ProgramEditorSnapshot, CompletePhysicalEffectWordsIncludeUnknownAndUnusedValues) {
    for (const auto revision : {2U, 4U}) {
        const auto snapshot = editor_snapshot(revision);
        const auto result = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, true);
        const auto &values = result.at("values");
        for (std::size_t slot = 0; slot < (revision == 4U ? 6U : 3U); ++slot) {
            for (std::size_t word = 0; word < 16U; ++word) {
                const auto key = "effects." + std::to_string(slot + 1U) + ".words." + std::to_string(word);
                ASSERT_TRUE(values.contains(key)) << key;
                EXPECT_EQ(values.at(key), 0x8100U + slot * 16U + word);
            }
        }
        EXPECT_FALSE(values.contains("effects.0.words.0"));
        EXPECT_FALSE(values.contains("effects.1.words.16"));
        if (revision == 2U)
            EXPECT_FALSE(values.contains("effects.4.words.0"));
    }
}

TEST(ProgramEditorSnapshot, AssignmentOrdinalsRetainDuplicateTargetsAndExcludeDormantCapacity) {
    for (const auto revision : {2U, 4U}) {
        const auto snapshot = editor_snapshot(revision);
        const auto result = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, true);
        const auto &assignments = result.at("assignments");
        ASSERT_EQ(assignments.size(), 3U);
        for (std::size_t ordinal = 0; ordinal < 3U; ++ordinal) {
            EXPECT_EQ(assignments.at(ordinal).at("ordinal"), ordinal);
            EXPECT_EQ(assignments.at(ordinal).at("kind"), ordinal < 2U ? "SBNK" : "SBAC");
            EXPECT_EQ(assignments.at(ordinal).at("name"), ordinal < 2U ? "Duplicate" : "Bank");
            EXPECT_EQ(result.at("values").at("assignments." + std::to_string(ordinal) + ".level_offset"),
                      11U + ordinal);
        }
        EXPECT_FALSE(result.at("values").contains("assignments.3.level_offset"));
    }
}

TEST(ProgramEditorSnapshot, NativeSwitchAndReceiveSelectionsUseFlatNumericValues) {
    auto snapshot = editor_snapshot(2U);
    constexpr std::array<std::uint8_t, 3> flags{0xffU, 0x3fU, 0x7fU};
    constexpr std::array<std::uint8_t, 3> receive{0xffU, 16U, 15U};
    for (std::size_t ordinal = 0; ordinal < 3U; ++ordinal) {
        snapshot.raw_payload[0x120U + ordinal * 0x38U + 0x23U] = static_cast<std::byte>(flags[ordinal]);
        snapshot.raw_payload[0x120U + ordinal * 0x38U + 0x15U] = static_cast<std::byte>(receive[ordinal]);
    }
    redecode(snapshot);
    const auto result = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, true);
    const auto &values = result.at("values");
    EXPECT_EQ(values.at("assignments.0.velocity_crossfade"), -1);
    EXPECT_EQ(values.at("assignments.1.velocity_crossfade"), 0);
    EXPECT_EQ(values.at("assignments.2.velocity_crossfade"), 1);
    EXPECT_EQ(values.at("assignments.0.receive"), -1);
    EXPECT_EQ(values.at("assignments.1.receive"), 16);
    EXPECT_EQ(values.at("assignments.2.receive"), 15);
}

TEST(ProgramEditorSnapshot, RevisionOneAndReadOnlyMediaRetainInspectionWithoutWriteAdmission) {
    for (const auto revision : {1U, 2U, 4U}) {
        const auto snapshot = editor_snapshot(revision);
        for (const bool writable : {false, true}) {
            const auto result = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, writable);
            ASSERT_TRUE(result.is_object());
            const bool editable = revision != 1U && writable;
            EXPECT_EQ(result.at("editable"), editable);
            EXPECT_EQ(result.at("reason").get<std::string>().empty(), editable);
            EXPECT_EQ(result.at("storageRevision"), revision);
            EXPECT_EQ(result.at("assignments").size(), 3U);
            EXPECT_TRUE(result.at("values").contains("effects.1.words.15"));
        }
    }
}

TEST(ProgramEditorSnapshot, MissingPlacementAndInvalidNumericHeaderCannotEdit) {
    auto snapshot = editor_snapshot(2U);
    snapshot.placement.reset();
    const auto unplaced = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, true);
    EXPECT_EQ(unplaced.at("editable"), false);
    EXPECT_FALSE(unplaced.at("reason").get<std::string>().empty());
    EXPECT_EQ(unplaced.at("volumeName"), "");
    for (const auto *name : {"", "000", "129", "33suffix"}) {
        snapshot = editor_snapshot(2U);
        snapshot.object.header.name = name;
        const auto result = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, true);
        EXPECT_EQ(result.at("editable"), false);
        EXPECT_TRUE(result.at("programNumber").is_null());
        EXPECT_FALSE(result.at("reason").get<std::string>().empty());
    }
}

TEST(ProgramEditorCatalog, NativeAndCurrentTypesHaveCompleteTypedPhysicalMetadata) {
    const auto catalog = axk::app::program_editor_catalog();
    EXPECT_EQ(catalog.at("schemaVersion"), 1U);
    ASSERT_EQ(catalog.at("formats").size(), 2U);
    std::set<std::string> models;
    for (const auto &format : catalog.at("formats")) {
        const auto model = format.at("model").get<std::string>();
        ASSERT_TRUE(model == "A3000" || model == "A5000");
        EXPECT_TRUE(models.insert(model).second);
        const auto profile = model == "A3000" ? axk::EffectProfile::a3000 : axk::EffectProfile::a5000;
        const auto count = model == "A3000" ? 55U : 97U;
        ASSERT_TRUE(format.at("fields").is_array());
        EXPECT_FALSE(format.at("fields").empty());
        ASSERT_EQ(format.at("effects").size(), count);
        std::set<std::uint16_t> ids;
        for (const auto &effect : format.at("effects")) {
            const auto id = effect.at("id").get<std::uint16_t>();
            EXPECT_LT(id, count);
            EXPECT_TRUE(ids.insert(id).second);
            EXPECT_FALSE(effect.at("label").get<std::string>().empty());
            EXPECT_TRUE(effect.at("printedNumber").is_number_unsigned() ||
                        effect.at("printedNumber").is_number_integer());
            const auto domain = axk::effect_write_info(id, profile);
            ASSERT_TRUE(domain);
            ASSERT_EQ(effect.at("resetWords").size(), 16U);
            ASSERT_EQ(effect.at("parameters").size(), 16U);
            for (std::size_t word = 0; word < 16U; ++word) {
                EXPECT_EQ(effect.at("resetWords").at(word), domain->reset_words[word]);
                const auto &parameter = effect.at("parameters").at(word);
                EXPECT_EQ(parameter.at("index"), word);
                EXPECT_TRUE(parameter.at("label").is_string());
                EXPECT_EQ(parameter.at("min"), domain->parameters[word].minimum);
                EXPECT_EQ(parameter.at("max"), domain->parameters[word].maximum);
                EXPECT_EQ(parameter.at("editable"),
                          domain->parameters[word].kind == axk::EffectParameterKind::stored_value);
            }
        }
    }
    EXPECT_EQ(models, (std::set<std::string>{"A3000", "A5000"}));
}

TEST(ProgramEditorCatalog, NativeHiddenResetWordDoesNotBecomeAnEditableParameter) {
    const auto catalog = axk::app::program_editor_catalog();
    bool checked_native = false;
    bool checked_current = false;
    for (const auto &format : catalog.at("formats")) {
        for (const auto &effect : format.at("effects")) {
            if (effect.at("id") != 54U)
                continue;
            const bool native = format.at("model") == "A3000";
            EXPECT_EQ(effect.at("resetWords").at(15), native ? 0x890aU : 0U);
            EXPECT_EQ(effect.at("parameters").at(15).at("editable"), false);
            if (native)
                checked_native = true;
            else
                checked_current = true;
        }
    }
    EXPECT_TRUE(checked_native);
    EXPECT_TRUE(checked_current);
}

TEST(ProgramEditorCatalog, EveryDecodedScalarHasOneMatchingModelField) {
    const auto catalog = axk::app::program_editor_catalog();
    for (const auto &format : catalog.at("formats")) {
        const bool native = format.at("model") == "A3000";
        const auto snapshot = editor_snapshot(native ? 2U : 4U);
        const auto editor = axk::app::detail::a_series_program_editor(snapshot, snapshot.raw_payload, true);
        std::set<std::string> keys;
        for (const auto &field : format.at("fields")) {
            EXPECT_TRUE(keys.insert(field.at("key").get<std::string>()).second);
            EXPECT_LE(field.at("min").get<int>(), field.at("max").get<int>());
        }
        for (const auto &[key, value] : editor.at("values").items()) {
            static_cast<void>(value);
            if (key.find(".words.") != std::string::npos)
                continue;
            auto field_key = key;
            if (key.starts_with("assignments.")) {
                const auto suffix = key.find('.', 12U);
                ASSERT_NE(suffix, std::string::npos);
                field_key = "assignments.*" + key.substr(suffix);
            }
            EXPECT_TRUE(keys.contains(field_key)) << format.at("model") << ": " << key;
        }
        EXPECT_EQ(keys.contains("assignments.*.velocity_crossfade"), native);
        EXPECT_EQ(keys.contains("assignments.*.velocity_sensitivity_offset"), !native);
        EXPECT_EQ(keys.contains("step_wave.step_count"), !native);
        EXPECT_EQ(keys.contains("effects.4.type"), !native);
        EXPECT_EQ(keys.contains("controller_reset.b.1"), !native);
    }
}

TEST(ProgramEditorSnapshot, ContextUsesResolvedIdsAndExcludesOtherVolumesWithoutReadingWaveData) {
    auto program = editor_snapshot(4U);
    axk::SampleSpec spec;
    spec.name = "Duplicate";
    spec.waveform_id = "wave";
    spec.parameters.aeg.attack_rate = 50U;
    auto sample_bytes = axk::detail::prepare_sbnk_payload(spec, {"MissingWave", 0x100U, 44100U, 1000U}, std::nullopt);
    ASSERT_TRUE(sample_bytes);
    auto sample = program;
    sample.key = "sample";
    sample.object = axk::decode_object(*sample_bytes).value();
    auto outside = sample;
    outside.placement->volume_directory = axk::SfsId{99U};
    std::unordered_map<std::string, axk::ObjectSnapshot> objects{
        {"program", program}, {"sample", sample}, {"outside", outside}};
    axk::app::ImageRelationshipItem assignment;
    assignment.source_object_id = "program";
    assignment.target_object_id = "sample";
    assignment.type = "PROG_ASSIGNMENT_TO_SBNK";
    assignment.assignment_index = 1U;
    auto membership = assignment;
    membership.source_object_id = "bank";
    membership.type = "SBAC_SLOT_TO_SBNK";
    nlohmann::ordered_json editor = axk::app::detail::a_series_program_editor(program, program.raw_payload, true);
    axk::app::detail::add_program_editing_context(editor, "program", objects, std::array{assignment, membership});
    EXPECT_TRUE(editor["assignments"][0]["targetObjectId"].is_null());
    EXPECT_EQ(editor["assignments"][1]["targetObjectId"], "sample");
    ASSERT_EQ(editor["targets"].size(), 1U);
    const auto &target = editor["targets"][0];
    EXPECT_EQ(target["objectId"], "sample");
    EXPECT_FALSE(target["assignable"].get<bool>());
    EXPECT_TRUE(target["available"].get<bool>());
    EXPECT_EQ(target["values"]["aeg.attack_rate"], 50U);
    EXPECT_FALSE(target.contains("sources"));
}

TEST(ProgramEditorSnapshot, PreservedBankRootSelectorDoesNotBecomeAPreviewOverride) {
    const auto program = editor_snapshot(4U);
    for (const auto format : {axk::SampleStorageFormat::a3000_188, axk::SampleStorageFormat::a4000_a5000_224}) {
        axk::SampleSpec member;
        member.name = "Member";
        axk::SampleBankSpec spec{"Bank", {"Member"}};
        spec.storage_format = format;
        auto bytes = axk::detail::prepare_sbac_payload(spec, {{"Member", member}});
        ASSERT_TRUE(bytes);
        (*bytes)[0x137] = std::byte{0x40};
        (*bytes)[0xa6] = std::byte{67};
        auto bank = program;
        bank.key = "bank";
        bank.raw_payload = *bytes;
        bank.object = axk::decode_object(*bytes).value();
        std::unordered_map<std::string, axk::ObjectSnapshot> objects{{"program", program}, {"bank", bank}};
        nlohmann::ordered_json editor = axk::app::detail::a_series_program_editor(program, program.raw_payload, true);
        axk::app::detail::add_program_editing_context(editor, "program", objects, {});
        ASSERT_EQ(editor["targets"].size(), 1U);
        const auto &target = editor["targets"][0];
        EXPECT_TRUE(target["available"].get<bool>());
        EXPECT_EQ(target["values"]["root_key"].get<unsigned>(), 67U);
        EXPECT_TRUE(target["overrideKeys"].empty());
        std::get<axk::CurrentSbac>(objects.at("bank").object.payload).override_enable_words[0] |= 1U;
        axk::app::detail::add_program_editing_context(editor, "program", objects, {});
        EXPECT_FALSE(editor["targets"][0]["available"].get<bool>());
    }
}

TEST(ProgramEditorCatalog, NewAssignmentDefaultsMatchTheNeutralWriterRow) {
    auto snapshot = editor_snapshot(4U);
    const auto neutral = axk::detail::prepare_prog_payload({1U, "Default", {{"SBNK", "Sample"}}});
    ASSERT_TRUE(neutral);
    snapshot.object = axk::decode_object(*neutral).value();
    const auto editor = axk::app::detail::a_series_program_editor(snapshot, *neutral, true);
    const auto catalog = axk::app::program_editor_catalog();
    for (const auto &field : catalog["formats"][1]["fields"]) {
        const auto key = field["key"].get<std::string>();
        if (!key.starts_with("assignments.*."))
            continue;
        EXPECT_EQ(field["defaultValue"], editor["values"]["assignments.0." + key.substr(14U)]) << key;
    }
}

} // namespace
