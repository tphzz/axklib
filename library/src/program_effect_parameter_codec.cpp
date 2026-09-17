#include "axklib/program_parameter_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "axklib/bytes.hpp"
#include "axklib/effects.hpp"

namespace axk::detail {
namespace {

template <typename T> bool outside(const std::optional<T> &value, int minimum, int maximum) {
    return value && (static_cast<int>(*value) < minimum || static_cast<int>(*value) > maximum);
}

template <typename T> std::optional<T> known(T value, int minimum, int maximum) {
    return static_cast<int>(value) < minimum || static_cast<int>(value) > maximum ? std::nullopt
                                                                                  : std::optional<T>{value};
}

template <typename T> void put(std::span<std::byte> bytes, std::size_t offset, const std::optional<T> &value) {
    if (value)
        bytes[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(*value));
}

Error invalid(const char *message) { return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, message); }

} // namespace

bool has_program_effect_parameter_values(const ProgramEffectParameters &value) {
    return value.enabled || value.input_level || value.output_level || value.pan || value.width || value.destination ||
           value.type || std::ranges::any_of(value.parameters, [](const auto &word) { return word.has_value(); });
}

ProgramEffectParameters decode_program_effect_parameters(const ProgEffectBlock &effect, std::size_t slot,
                                                         ProgramParameterGeneration generation) {
    const auto a3000 = generation == ProgramParameterGeneration::a3000;
    const ByteReader reader{effect.raw_bytes};
    ProgramEffectParameters result;
    if (*reader.u8(0) <= 1U)
        result.enabled = *reader.u8(0) != 0U;
    result.input_level = known(*reader.u8(1), 0, 127);
    result.output_level = known(*reader.u8(2), 0, 127);
    result.pan = known(*reader.s8(3), -63, 63);
    result.destination = known(*reader.u8(4), 0, !a3000 && slot < 3U ? 8 : 5);
    result.width = known(*reader.s8(5), -126, 0);
    const auto info = effect_write_info(effect.type, a3000 ? EffectProfile::a3000 : EffectProfile::a4000);
    if (!info)
        return result;
    result.type = static_cast<std::uint8_t>(effect.type);
    for (std::size_t index = 0; index < result.parameters.size(); ++index) {
        const auto &domain = info->parameters[index];
        if (domain.kind == EffectParameterKind::stored_value)
            result.parameters[index] = known(effect.parameter_values[index], domain.minimum, domain.maximum);
    }
    return result;
}

Result<void> apply_effect_parameter_block(std::span<std::byte, 40> block, const ProgramEffectParameters &value,
                                          ASeriesModel model, std::size_t slot, EffectBlockKind kind,
                                          ProgramParameterWriteMode mode) {
    const auto native = model == ASeriesModel::a3000;
    const auto recording = kind == EffectBlockKind::recording;
    if ((model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000) ||
        (native && kind == EffectBlockKind::program) || slot >= (recording || model != ASeriesModel::a5000 ? 3U : 6U))
        return std::unexpected{invalid("Effect block is not supported for the target model")};
    if (!has_program_effect_parameter_values(value))
        return {};
    if (outside(value.input_level, 0, 127) || outside(value.output_level, 0, 127) || outside(value.pan, -63, 63) ||
        outside(value.width, -126, 0) ||
        outside(value.destination, 0, model == ASeriesModel::a5000 && slot < 3U ? 8 : 5) ||
        outside(value.type, 0, native ? 54 : 96))
        return std::unexpected{invalid("Effect parameter is outside its supported domain")};
    const auto type_offset = native ? 7U : 6U;
    const auto old_type = std::to_integer<std::uint8_t>(block[type_offset]);
    const auto effective_type = value.type.value_or(old_type);
    const auto info = effect_write_info(effective_type, native ? EffectProfile::a3000 : EffectProfile::a4000);
    for (std::size_t index = 0; index < value.parameters.size(); ++index) {
        const auto &word = value.parameters[index];
        if (!word)
            continue;
        if (!info || info->parameters[index].kind != EffectParameterKind::stored_value)
            return std::unexpected{invalid("Effect action, unused, or unknown parameter slots cannot be authored")};
        const auto &domain = info->parameters[index];
        if (*word < domain.minimum || *word > domain.maximum)
            return std::unexpected{invalid("Effect parameter word is outside the selected type's numeric domain")};
    }
    ByteWriter writer{block};
    if (value.type && (mode == ProgramParameterWriteMode::fresh || *value.type != old_type)) {
        for (std::size_t index = 0; index < info->reset_words.size(); ++index) {
            if (auto written = writer.write_be16(8U + index * 2U, info->reset_words[index]); !written)
                return written;
        }
        if (!native && !recording && slot < 3U)
            block[7] = static_cast<std::byte>(info->legacy_type);
    }
    put(block, 0, value.enabled);
    put(block, 1, value.input_level);
    put(block, 2, value.output_level);
    put(block, 3, value.pan);
    put(block, 4, value.destination);
    put(block, 5, value.width);
    put(block, type_offset, value.type);
    for (std::size_t index = 0; index < value.parameters.size(); ++index) {
        if (value.parameters[index]) {
            if (auto written = writer.write_be16(8U + index * 2U, *value.parameters[index]); !written)
                return written;
        }
    }
    return {};
}

Result<void> apply_program_effect_parameters(std::span<std::byte> bytes, const ProgLayout &layout,
                                             const ProgramParameters &parameters, ASeriesModel model,
                                             ProgramParameterWriteMode mode) {
    for (std::size_t slot = 0; slot < parameters.effects.size(); ++slot) {
        const auto &value = parameters.effects[slot];
        if (!has_program_effect_parameter_values(value))
            continue;
        if (model != ASeriesModel::a5000 && slot >= 3U)
            return std::unexpected{invalid("Effects 4..6 require an A5000 target model")};
        const auto offset = slot < 3U ? 0x98U + slot * 0x28U : *layout.parameter_tail_offset + (slot - 3U) * 0x28U;
        if (auto written = apply_effect_parameter_block(bytes.subspan(offset).first<40>(), value, model, slot,
                                                        EffectBlockKind::program, mode);
            !written)
            return written;
    }
    return {};
}

} // namespace axk::detail
