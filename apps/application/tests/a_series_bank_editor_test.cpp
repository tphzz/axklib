#include <algorithm>
#include <filesystem>
#include <string>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a_series_sample_editor.hpp"
#include "axklib/catalog.hpp"
#include "axklib/object.hpp"

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
