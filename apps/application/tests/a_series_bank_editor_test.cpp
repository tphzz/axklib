#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a_series_sample_editor.hpp"
#include "axklib/application/sample_formats.hpp"
#include "axklib/catalog.hpp"
#include "axklib/object.hpp"

TEST(SampleBankFormats, DiagnosticsIgnoreInactiveAndSampleOnlyParametersWithoutChangingBytes) {
    axk::CurrentSbac bank;
    bank.storage.format = axk::SampleStorageFormat::a4000_a5000_224;
    bank.storage.structurally_valid = true;
    bank.storage.parameter_bytes = 224U;
    bank.raw_sample_parameter_block[0x2eU] = std::byte{255};
    bank.raw_sample_parameter_block[0x9bU] = std::byte{3};
    bank.raw_sample_parameter_block[0xd6U] = std::byte{10};
    const auto bytes = bank.raw_sample_parameter_block;
    const auto inactive = axk::app::sample_format_metadata(bank);
    EXPECT_TRUE(inactive.at("parameterIssues").empty());
    EXPECT_FALSE(inactive.at("requiresA5000"));
    bank.override_enable_words[2] = (1U << (69U % 32U)) | (1U << (79U % 32U));
    const auto active = axk::app::sample_format_metadata(bank);
    ASSERT_EQ(active.at("parameterIssues").size(), 1U);
    EXPECT_EQ(active.at("parameterIssues")[0].at("key"), "aeg.attack_mode");
    EXPECT_TRUE(active.at("requiresA5000"));
    EXPECT_EQ(bank.raw_sample_parameter_block, bytes);
}

TEST(SampleBankFormats, PartialGroupedSelectorsAndUnsupportedFlagsRetainTheirDiagnostics) {
    axk::CurrentSbac bank;
    bank.storage.format = axk::SampleStorageFormat::a4000_a5000_224;
    bank.storage.structurally_valid = true;
    bank.storage.parameter_bytes = 224U;
    bank.raw_sample_parameter_block[0x7aU] = std::byte{4};
    bank.raw_sample_parameter_block[0x7bU] = std::byte{77};
    bank.raw_sample_parameter_block[0x7cU] = std::byte{10};
    bank.override_enable_words[2] = 1U << (85U % 32U);
    const auto active = axk::app::sample_format_metadata(bank);
    ASSERT_EQ(active.at("parameterIssues").size(), 1U);
    EXPECT_EQ(active.at("parameterIssues")[0].at("key"), "sample_eq_gain_db");
    bank.override_enable_words[2] |= 0x80000000U;
    EXPECT_FALSE(axk::app::sample_format_metadata(bank).at("diagnostics").empty());
}

TEST(SampleBankEditor, PreservedRootSelectorAllowsEditingButNeverExposesRootAsAnOverride) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} /
                      "tests/fixtures/images/sampler-authored/HD00_512_multi_sbnk_authored.hds";
    const auto image = axk::open_image(path);
    ASSERT_TRUE(image);
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    const auto found = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return std::holds_alternative<axk::CurrentSbac>(object.object.payload);
    });
    ASSERT_NE(found, catalog->objects.end());
    for (const auto format : {axk::SampleStorageFormat::a3000_188, axk::SampleStorageFormat::a4000_a5000_224}) {
        auto snapshot = *found;
        auto &bank = std::get<axk::CurrentSbac>(snapshot.object.payload);
        bank.storage.format = format;
        bank.storage.parameter_bytes = format == axk::SampleStorageFormat::a3000_188 ? 188U : 224U;
        bank.override_enable_words = {1U << 6U, 0U, 0U};
        bank.raw_sample_parameter_block[0x2e] = std::byte{255};
        const auto original = bank.raw_sample_parameter_block;
        const auto editing = [&](bool writable = true) {
            return axk::app::detail::a_series_bank_editor(snapshot, snapshot.raw_payload, writable,
                                                          nlohmann::json::array());
        };
        const auto supported = editing();
        ASSERT_TRUE(supported.at("editable").get<bool>());
        EXPECT_TRUE(supported.at("reason").get<std::string>().empty());
        EXPECT_TRUE(supported.at("sampleFormat").at("diagnostics").empty());
        EXPECT_TRUE(supported.at("sampleFormat").at("parameterIssues").empty());
        const auto &blocked = supported.at("blockedParameters");
        EXPECT_NE(std::ranges::find(blocked, "root_key"), blocked.end());
        for (const auto &unit : supported.at("bankOverrides").at("units")) {
            EXPECT_NE(unit.at("id").get<unsigned>(), 6U);
            EXPECT_EQ(std::ranges::find(unit.at("keys"), "root_key"), unit.at("keys").end());
        }
        EXPECT_FALSE(editing(false).at("editable").get<bool>());
        bank.override_enable_words[2] |= 1U << 31U;
        EXPECT_FALSE(editing().at("editable").get<bool>());
        EXPECT_FALSE(editing().at("sampleFormat").at("diagnostics").empty());
        bank.override_enable_words[2] = 0U;
        snapshot.placement.reset();
        EXPECT_FALSE(editing().at("editable").get<bool>());
        bank.storage.structurally_valid = false;
        EXPECT_TRUE(editing().is_null());
        EXPECT_EQ(bank.raw_sample_parameter_block, original);
    }
}

TEST(SampleBankEditor, EmptyUnresolvedAndInactiveMembersDoNotBlockBankOnlyEditing) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} /
                      "tests/fixtures/images/sampler-authored/HD00_512_multi_sbnk_authored.hds";
    const auto image = axk::open_image(path);
    ASSERT_TRUE(image);
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    const auto found = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return std::holds_alternative<axk::CurrentSbac>(object.object.payload);
    });
    ASSERT_NE(found, catalog->objects.end());
    auto snapshot = *found;
    auto &bank = std::get<axk::CurrentSbac>(snapshot.object.payload);
    bank.override_enable_words = {};
    ASSERT_FALSE(bank.slots.empty());
    const auto editing = [&] {
        return axk::app::detail::a_series_bank_editor(snapshot, snapshot.raw_payload, true, nlohmann::json::array());
    };
    const auto unresolved = editing();
    EXPECT_TRUE(unresolved.at("editable"));
    EXPECT_FALSE(unresolved.at("canEditPlayback"));
    for (const auto &member : unresolved.at("bankOverrides").at("members"))
        EXPECT_TRUE(member.at("objectId").is_null());
    bank.slots.front().active = false;
    EXPECT_EQ(editing().at("bankOverrides").at("members").size(),
              unresolved.at("bankOverrides").at("members").size() - 1U);
    bank.slots.clear();
    bank.stored_member_count = 0U;
    bank.effective_member_count = 0U;
    EXPECT_TRUE(editing().at("editable"));
    EXPECT_TRUE(editing().at("bankOverrides").at("members").empty());
    bank.override_enable_words[2] = 0x80000000U;
    const auto unsupported = editing();
    EXPECT_FALSE(unsupported.at("editable"));
    EXPECT_FALSE(unsupported.at("reason").get<std::string>().empty());
}
