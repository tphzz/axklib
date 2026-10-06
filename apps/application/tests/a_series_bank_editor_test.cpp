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
