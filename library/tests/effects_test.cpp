#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

#include "axklib/effects.hpp"

TEST(Effects, NumericWriteMetadataCoversOrdinaryTypesAndHiddenDefaults) {
    for (std::uint16_t type = 0; type <= 96; ++type) {
        const auto info = axk::effect_write_info(type);
        ASSERT_TRUE(info);
        EXPECT_EQ(info->legacy_type, type <= 54U ? type : 0U);
        for (std::size_t index = 0; index < info->parameters.size(); ++index) {
            const auto &domain = info->parameters[index];
            if (domain.kind != axk::EffectParameterKind::stored_value)
                continue;
            EXPECT_LE(domain.minimum, info->reset_words[index]);
            EXPECT_GE(domain.maximum, info->reset_words[index]);
        }
    }
    EXPECT_FALSE(axk::effect_write_info(97));
    EXPECT_FALSE(axk::effect_write_info(65535));
    const auto hall = axk::effect_write_info(47);
    ASSERT_TRUE(hall);
    const std::array<std::uint16_t, 16> expected{50, 18, 10, 8, 13, 49, 0, 4, 50, 8, 64, 5, 5, 5, 5, 5};
    EXPECT_EQ(hall->reset_words, expected);
    EXPECT_EQ(hall->parameters[11].kind, axk::EffectParameterKind::unused);
    const auto scratch = axk::effect_write_info(1);
    ASSERT_TRUE(scratch);
    EXPECT_EQ(scratch->reset_words[1], 1800);
    EXPECT_EQ(scratch->reset_words[15], 3);
    const auto distortion = axk::effect_write_info(67);
    ASSERT_TRUE(distortion);
    EXPECT_EQ(distortion->parameters[3].maximum, 20);
    EXPECT_EQ(distortion->reset_words[15], 35082);
}

TEST(Effects, ExposesProfilesTypesParametersAndModelRequirements) {
    EXPECT_EQ(axk::parse_effect_profile("auto"), axk::EffectProfile::a4000);
    EXPECT_EQ(axk::parse_effect_profile("a3000"), axk::EffectProfile::a3000);
    EXPECT_FALSE(axk::parse_effect_profile("unknown"));
    EXPECT_TRUE(axk::effect_type_supported(53, axk::EffectProfile::a3000));
    EXPECT_FALSE(axk::effect_type_supported(54, axk::EffectProfile::a3000));
    EXPECT_TRUE(axk::effect_type_supported(97, axk::EffectProfile::a4000));
    const auto type = axk::effect_type_info(2);
    ASSERT_TRUE(type);
    EXPECT_EQ(type->ui_label, "002/AutoSyn");
    const auto parameter = axk::effect_parameter_info(2, 5);
    ASSERT_TRUE(parameter);
    EXPECT_EQ(parameter->parameter_label, "LPF Frequency");
    EXPECT_EQ(axk::effect_slot_requirement(4).requirement, "a5000_only");
    EXPECT_EQ(axk::effect_output_destination_requirement(10).requirement, "a5000_only");
}

TEST(Effects, MatchesValidatedEnumNumericAndBoundaryDisplays) {
    const auto known = axk::format_effect_parameter(2, 5, 40);
    EXPECT_EQ(known.value, "2.0kHz");
    EXPECT_EQ(known.quality, "Known");
    EXPECT_EQ(known.table_index, 6);

    const auto enum_value = axk::format_effect_parameter(2, 2, 2);
    EXPECT_EQ(enum_value.value, "TypeC");
    EXPECT_EQ(enum_value.quality, "Likely");

    const auto signed_value = axk::format_effect_parameter(2, 10, 80);
    EXPECT_EQ(signed_value.value, "+16");
    const auto unsupported = axk::format_effect_parameter(83, 1, 36, axk::EffectProfile::a3000);
    EXPECT_TRUE(unsupported.value.empty());
    EXPECT_EQ(unsupported.quality, "Unknown");
    const auto missing = axk::format_effect_parameter(std::nullopt, 1, 0);
    EXPECT_TRUE(missing.value.empty());
    EXPECT_FALSE(missing.table_index);
}

TEST(Effects, PreservesWideStoredParameterValues) {
    const auto delay = axk::format_effect_parameter(1, 2, std::optional<std::uint16_t>{4600});
    EXPECT_EQ(delay.value, "460ms");
    EXPECT_EQ(delay.table_index, 4599);
    const auto offset = axk::format_effect_parameter(2, 9, std::optional<std::uint16_t>{884});
    EXPECT_EQ(offset.value, "-884");
}

TEST(Effects, DistinguishesStoredDomainsActionsAndUnusedSlots) {
    const auto frequency = axk::effect_parameter_info(36, 7);
    ASSERT_TRUE(frequency);
    EXPECT_EQ(frequency->raw_min, "28");
    EXPECT_EQ(frequency->raw_max, "58");
    EXPECT_EQ(axk::effect_parameter_info(67, 4)->raw_max, "20");
    EXPECT_EQ(axk::effect_parameter_info(88, 10)->raw_max, "64");
    EXPECT_EQ(axk::effect_parameter_info(91, 7)->raw_max, "3");
    for (const auto &[type, number] : std::array<std::pair<std::uint16_t, std::uint8_t>, 7>{
             {{91, 15}, {92, 13}, {93, 11}, {94, 14}, {94, 15}, {95, 16}, {96, 16}}}) {
        const auto parameter = axk::effect_parameter_info(type, number);
        ASSERT_TRUE(parameter);
        EXPECT_EQ(parameter->kind, axk::EffectParameterKind::control_action);
        EXPECT_EQ(parameter->raw_max, "0");
        EXPECT_FALSE(axk::format_effect_parameter(type, number, 0).table_index);
    }
    for (const auto &[type, first] :
         std::array<std::pair<std::uint16_t, std::uint8_t>, 3>{{{39, 13}, {63, 13}, {72, 11}}}) {
        for (auto number = first; number <= 16U; ++number) {
            const auto parameter = axk::effect_parameter_info(type, number);
            ASSERT_TRUE(parameter);
            EXPECT_EQ(parameter->kind, axk::EffectParameterKind::unused);
            EXPECT_FALSE(axk::format_effect_parameter(type, number, 0).table_index);
        }
    }
}
