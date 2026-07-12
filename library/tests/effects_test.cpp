#include <cstdint>

#include <gtest/gtest.h>

#include "axklib/effects.hpp"

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
