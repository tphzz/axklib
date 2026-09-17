#include "axklib/system_file.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "axklib/effects.hpp"

namespace axk {
namespace {

std::size_t row_index(std::uint16_t raw_type, bool native) {
    return native ? raw_type : effect_type_info(raw_type)->printed_number;
}

SystemEffectFavorites effect_preferences(std::uint16_t raw_type, bool native) {
    const auto info = effect_write_info(raw_type, native ? EffectProfile::a3000 : EffectProfile::a4000).value();
    const auto count = static_cast<std::uint8_t>(std::ranges::count_if(
        info.parameters, [](const auto &parameter) { return parameter.kind != EffectParameterKind::unused; }));
    const auto positions = count == 0U ? 0U : native ? std::min<unsigned>(count, 4U) : 4U;
    return {raw_type, count, static_cast<std::uint8_t>(positions), {}};
}

Error invalid(const char *message) { return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message); }

} // namespace

Result<DecodedSystemFavorites> decode_system_favorites(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a3000_system && file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Favorites System File kind is unsupported")};
    const auto native = file.kind == SystemFileKind::a3000_system;
    if (file.storage_revision > (native ? 0U : 1U) || file.system_bulk_bytes.size() != (native ? 0x348U : 0xfe0U))
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::object, "Favorites System File layout is invalid")};
    const auto bytes = std::span{file.system_bulk_bytes}.subspan(native ? 0x50U : 0x1e0U, native ? 128U : 256U);
    DecodedSystemFavorites result;
    result.raw_bytes.assign(bytes.begin(), bytes.end());
    const auto count = native ? 55U : 97U;
    result.effects.reserve(count);
    for (std::uint16_t raw_type = 0; raw_type < count; ++raw_type) {
        auto effect = effect_preferences(raw_type, native);
        const auto row = row_index(raw_type, native);
        for (std::size_t position = 0; position < 4U; ++position) {
            const auto byte = std::to_integer<unsigned>(bytes[row * 2U + position / 2U]);
            effect.selections[position] = static_cast<std::uint8_t>((byte >> (position % 2U == 0U ? 4U : 0U)) & 15U);
        }
        result.effects.push_back(effect);
    }
    return result;
}

Result<DecodedSystemFile> patch_system_favorites(const DecodedSystemFile &file,
                                                 std::span<const SystemEffectFavoritesPatch> patches,
                                                 ASeriesModel model) {
    if (model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Favorites target model is unsupported")};
    const auto native = model == ASeriesModel::a3000;
    if (file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Favorites target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    const auto offset = current_record_envelope_size + (native ? 0x70U : 0x200U);
    auto bytes = std::span{*encoded}.subspan(offset, native ? 128U : 256U);
    std::array<bool, 97> seen{};
    for (const auto &patch : patches) {
        if (patch.raw_type >= (native ? 55U : 97U))
            return std::unexpected{invalid("Favorite effect type is unavailable on the target model")};
        if (seen[patch.raw_type])
            return std::unexpected{invalid("Favorite effect type occurs more than once in the patch")};
        seen[patch.raw_type] = true;
        const auto effect = effect_preferences(patch.raw_type, native);
        const auto row = row_index(patch.raw_type, native);
        for (std::size_t position = 0; position < patch.selections.size(); ++position) {
            if (!patch.selections[position])
                continue;
            if (position >= effect.editable_position_count || *patch.selections[position] >= effect.parameter_count)
                return std::unexpected{invalid("Favorite position or parameter selection is unavailable")};
            const auto shift = position % 2U == 0U ? 4U : 0U;
            auto &byte = bytes[row * 2U + position / 2U];
            byte = (byte & ~static_cast<std::byte>(15U << shift)) |
                   static_cast<std::byte>(static_cast<unsigned>(*patch.selections[position]) << shift);
        }
    }
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
