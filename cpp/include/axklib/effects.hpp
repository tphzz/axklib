#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/export.hpp"

namespace axk {

enum class EffectProfile : std::uint8_t { a3000, a4000, a5000 };

struct EffectTypeInfo {
  std::uint16_t raw_type{};
  std::uint16_t printed_number{};
  std::string_view printed_label;
  std::string_view ui_label;
  bool validated{};
};

struct EffectParameterInfo {
  std::uint16_t raw_type{};
  std::uint8_t parameter_number{};
  std::string_view effect_label;
  std::string_view parameter_label;
  std::string_view range_text;
  std::string_view raw_min;
  std::string_view raw_max;
  std::string_view raw_interval;
  std::string_view raw_scaler;
  std::string_view raw_shift;
  std::string_view value_source;
  std::string_view table_source;
};

struct EffectDisplayValue {
  std::string value;
  std::optional<std::int32_t> table_index;
  std::string quality;
  std::string source;
  std::string note;
};

struct ModelRequirement {
  std::string_view requirement;
  std::vector<std::string_view> compatible_models;
  std::string_view quality;
  std::string_view note;
};

AXK_API std::optional<EffectProfile> parse_effect_profile(std::string_view value) noexcept;
AXK_API bool effect_type_supported(std::uint16_t raw_type,
                                   EffectProfile profile = EffectProfile::a4000) noexcept;
AXK_API std::optional<EffectTypeInfo>
effect_type_info(std::uint16_t raw_type, EffectProfile profile = EffectProfile::a4000) noexcept;
AXK_API std::optional<EffectParameterInfo>
effect_parameter_info(std::uint16_t raw_type, std::uint8_t parameter_number,
                      EffectProfile profile = EffectProfile::a4000) noexcept;
AXK_API EffectDisplayValue format_effect_parameter(std::optional<std::uint16_t> raw_type,
                                                   std::uint8_t parameter_number,
                                                   std::optional<std::uint8_t> raw_value,
                                                   EffectProfile profile = EffectProfile::a4000);
AXK_API ModelRequirement effect_slot_requirement(std::uint8_t effect_number);
AXK_API ModelRequirement
effect_output_destination_requirement(std::optional<std::uint8_t> raw_value);

} // namespace axk
