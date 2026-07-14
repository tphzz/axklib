#include "axklib/effects.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>

#include "axklib/generated/current_effects.hpp"

namespace axk {
namespace {

constexpr std::string_view table_note =
    "Table value-label transform; use as Likely display metadata.";
constexpr std::string_view validated_note =
    "Validated Program Effect display transform.";

const generated::EffectTypeData *type_data(std::uint16_t raw_type) {
    const auto found = std::ranges::find(generated::effect_types, raw_type,
                                         &generated::EffectTypeData::raw_type);
    return found == generated::effect_types.end() ? nullptr : &*found;
}

const generated::EffectParameterData *
parameter_data(std::uint16_t raw_type, std::uint8_t parameter_number) {
    const auto found = std::ranges::find_if(
        generated::effect_parameters, [&](const auto &row) {
            return row.raw_type == raw_type &&
                   row.parameter_number == parameter_number;
        });
    return found == generated::effect_parameters.end() ? nullptr : &*found;
}

std::optional<std::int32_t> integer(std::string_view value) {
    std::int32_t result{};
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        return std::nullopt;
    return result;
}

std::optional<std::int32_t>
table_index(const generated::EffectParameterData &parameter,
            std::uint8_t raw_value) {
    const auto scaler = integer(parameter.raw_scaler);
    const auto shift = integer(parameter.raw_shift);
    if (!scaler || *scaler == 0 || !shift)
        return std::nullopt;
    return static_cast<std::int32_t>(raw_value) * *scaler + *shift;
}

std::string signed_label(std::int32_t value) {
    return value > 0 ? std::format("+{}", value) : std::to_string(value);
}

std::string db_label(std::int32_t value) {
    return value > 0 ? std::format("+{}dB", value) : std::format("{}dB", value);
}

std::string tenths_ms(std::int32_t tenths) {
    return tenths % 10 == 0
               ? std::format("{}ms", tenths / 10)
               : std::format("{:.1f}ms", static_cast<double>(tenths) / 10.0);
}

std::optional<std::string> numeric_display(std::string_view source,
                                           std::uint8_t raw_value,
                                           std::int32_t index) {
    if (source.starts_with("LSB_Value"))
        return std::to_string(raw_value);
    if (source == "Value_63_63" || source == "Value_64_63" ||
        source == "Mod_Depth_Ofst_R")
        return signed_label(index - 63);
    if (source == "Value_24_24")
        return signed_label(index - 24);
    if (source == "Fine_50_50")
        return signed_label(index - 50);
    if (source == "EQ_Gain" || source == "Output_Gain")
        return db_label(index - 12);
    if (source == "EQ_Width")
        return std::format("{:.1f}", static_cast<double>(index + 10) / 10.0);
    if (source.starts_with("Delay_01_") ||
        source.starts_with("DelayTime_01_") || source == "Initial_Delay")
        return tenths_ms(index + 1);
    if (source == "Delay_884_0ms")
        return index == 0 ? std::string{"0"} : std::format("-{}", index);
    if (source.starts_with("Delay_") && source.ends_with("ms"))
        return tenths_ms(index + 1);
    return std::nullopt;
}

const generated::KnownEffectDisplay *
known_display(std::uint16_t raw_type, std::uint8_t parameter_number,
              std::uint8_t raw_value) {
    const auto found = std::ranges::find_if(
        generated::known_effect_displays, [&](const auto &row) {
            return row.raw_type == raw_type &&
                   row.parameter_number == parameter_number &&
                   row.raw_value == raw_value;
        });
    return found == generated::known_effect_displays.end() ? nullptr : &*found;
}

std::optional<std::string_view> enum_label(std::string_view table,
                                           std::int32_t index) {
    if (index < 0)
        return std::nullopt;
    const auto found = std::ranges::find_if(
        generated::effect_enum_values, [&](const auto &row) {
            return row.table == table &&
                   row.index == static_cast<std::uint32_t>(index);
        });
    return found == generated::effect_enum_values.end()
               ? std::nullopt
               : std::optional<std::string_view>{found->label};
}

ModelRequirement shared_requirement() {
    return {"a4000_a5000",
            {"a4000", "a5000"},
            "Likely",
            "A4000/A5000 shared effect table or Effect1-3 slot."};
}

ModelRequirement a5000_requirement() {
    return {"a5000_only", {"a5000"}, "Likely", "A5000-only effect feature."};
}

ModelRequirement unknown_requirement() {
    return {"unknown",
            {},
            "Unknown",
            "No model compatibility rule is known for this value."};
}

} // namespace

std::optional<EffectProfile>
parse_effect_profile(std::string_view value) noexcept {
    if (value.empty() || value == "auto" || value == "a4000")
        return EffectProfile::a4000;
    if (value == "a3000")
        return EffectProfile::a3000;
    if (value == "a5000")
        return EffectProfile::a5000;
    return std::nullopt;
}

bool effect_type_supported(std::uint16_t raw_type,
                           EffectProfile profile) noexcept {
    return type_data(raw_type) != nullptr &&
           (profile != EffectProfile::a3000 || raw_type < 54U);
}

std::optional<EffectTypeInfo> effect_type_info(std::uint16_t raw_type,
                                               EffectProfile profile) noexcept {
    if (!effect_type_supported(raw_type, profile))
        return std::nullopt;
    const auto *row = type_data(raw_type);
    return EffectTypeInfo{row->raw_type, row->printed_number,
                          row->printed_label, row->ui_label, row->validated};
}

std::optional<EffectParameterInfo>
effect_parameter_info(std::uint16_t raw_type, std::uint8_t parameter_number,
                      EffectProfile profile) noexcept {
    if (!effect_type_supported(raw_type, profile))
        return std::nullopt;
    const auto *row = parameter_data(raw_type, parameter_number);
    if (row == nullptr)
        return std::nullopt;
    return EffectParameterInfo{
        row->raw_type,        row->parameter_number, row->effect_label,
        row->parameter_label, row->range_text,       row->raw_min,
        row->raw_max,         row->raw_interval,     row->raw_scaler,
        row->raw_shift,       row->value_source,     row->table_source};
}

EffectDisplayValue format_effect_parameter(
    std::optional<std::uint16_t> raw_type, std::uint8_t parameter_number,
    std::optional<std::uint8_t> raw_value, EffectProfile profile) {
    if (!raw_type || !raw_value)
        return {{},
                std::nullopt,
                "Unknown",
                {},
                "No raw effect type or parameter value available."};
    if (!effect_type_supported(*raw_type, profile))
        return {{},
                std::nullopt,
                "Unknown",
                {},
                "Raw effect type is not supported by the selected A-series "
                "profile."};
    const auto *parameter = parameter_data(*raw_type, parameter_number);
    const auto index = parameter == nullptr
                           ? std::nullopt
                           : table_index(*parameter, *raw_value);
    if (const auto *known =
            known_display(*raw_type, parameter_number, *raw_value))
        return {std::string{known->display}, index, "Known",
                "validated UI-load check", std::string{validated_note}};
    if (parameter == nullptr)
        return {{},
                std::nullopt,
                "Unknown",
                {},
                "No effect parameter table row for this raw type/parameter."};
    if (!index)
        return {{},
                std::nullopt,
                "Unknown",
                std::string{parameter->table_source},
                "No numeric table display transform is available."};
    if (*index < 0)
        return {{},
                index,
                "Unknown",
                std::string{parameter->table_source},
                "Computed table index is below zero."};
    if (const auto label = enum_label(parameter->value_source, *index))
        return {std::string{*label}, index, "Likely",
                std::format("{}:{}", parameter->table_source,
                            parameter->value_source),
                std::string{table_note}};
    const auto has_enum_table = std::ranges::any_of(
        generated::effect_enum_values,
        [&](const auto &row) { return row.table == parameter->value_source; });
    if (has_enum_table)
        return {{},
                index,
                "Unknown",
                std::string{parameter->table_source},
                "Computed table index is outside the label table."};
    if (const auto numeric =
            numeric_display(parameter->value_source, *raw_value, *index))
        return {*numeric, index, "Likely",
                std::format("{}:{}", parameter->table_source,
                            parameter->value_source),
                std::string{table_note}};
    return {{},
            index,
            "Unknown",
            std::string{parameter->table_source},
            std::format("No table display labels for value source {}.",
                        parameter->value_source)};
}

ModelRequirement effect_slot_requirement(std::uint8_t effect_number) {
    if (effect_number >= 4U)
        return a5000_requirement();
    if (effect_number >= 1U)
        return shared_requirement();
    return unknown_requirement();
}

ModelRequirement
effect_output_destination_requirement(std::optional<std::uint8_t> raw_value) {
    if (!raw_value)
        return unknown_requirement();
    if (*raw_value == 10U || *raw_value == 11U || *raw_value == 12U)
        return a5000_requirement();
    return shared_requirement();
}

} // namespace axk
