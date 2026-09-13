#include "axklib/system_file.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "axklib/bytes.hpp"
#include "system_global_fields.hpp"

namespace axk {
namespace {

template <typename T> std::optional<T> bounded(int value, int minimum, int maximum) {
    if (value < minimum || value > maximum)
        return std::nullopt;
    return static_cast<T>(value);
}

template <typename T> std::optional<T> scalar(const ByteReader &reader, std::size_t offset, int minimum, int maximum) {
    if constexpr (std::is_signed_v<T>)
        return bounded<T>(*reader.s8(offset), minimum, maximum);
    else
        return bounded<T>(*reader.u8(offset), minimum, maximum);
}

SystemGlobalParameters decode_parameters(std::span<const std::byte> bytes, bool native) {
    const ByteReader reader{bytes};
    SystemGlobalParameters p;
    detail::visit_system_global_scalars(
        p, !native,
        [&]<typename T>(std::size_t offset, std::optional<T> &value, int minimum, int maximum, const char *) {
            if (offset < bytes.size())
                value = scalar<T>(reader, offset, minimum, maximum);
        });
    const auto flags = *reader.u8(6);
    p.omni = (flags & 1U) != 0U;
    p.program_change_enabled = (flags & 2U) != 0U;
    p.wave_length_lock = (flags & 4U) != 0U;
    p.wave_auto_zero = (flags & 8U) != 0U;
    p.wave_auto_snap = (flags & 16U) != 0U;
    p.audition_with_easy_edit = (flags & 32U) != 0U;
    p.audition_with_effects = (flags & 64U) != 0U;
    p.play_and_load = (flags & 128U) != 0U;
    const auto remix = *reader.u8(0x24);
    p.remix_type_selection = bounded<std::uint8_t>(remix >> 4U, 0, native ? 4 : 9);
    p.remix_variation_selection = bounded<std::uint8_t>(remix & 15U, 0, native ? 3 : 7);
    if (native)
        return p;
    const auto extra_flags = *reader.u8(0x2f);
    p.remix_auto_audition = (extra_flags & 1U) != 0U;
    p.knob_midi_out = (extra_flags & 2U) != 0U;
    p.function_key_midi_out = (extra_flags & 4U) != 0U;
    return p;
}

} // namespace

Result<DecodedSystemGlobal> decode_system_global(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a3000_system && file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Global System File kind is unsupported")};
    const auto native = file.kind == SystemFileKind::a3000_system;
    if (file.storage_revision > (native ? 0U : 1U) || file.system_bulk_bytes.size() != (native ? 0x348U : 0xfe0U))
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::object, "Global System File layout is invalid")};
    const auto bytes = std::span{file.system_bulk_bytes}.subspan(0x10U, native ? 0x2eU : 0x1c0U);
    DecodedSystemGlobal result;
    result.raw_bytes.assign(bytes.begin(), bytes.end());
    result.parameters = decode_parameters(bytes, native);
    if (!native) {
        auto &recipes = result.registered_remix.emplace();
        for (std::size_t slot = 0; slot < recipes.size(); ++slot) {
            for (std::size_t i = 0; i < 24U; ++i) {
                recipes[slot].duration_codes[i] = std::to_integer<std::uint8_t>(bytes[0x50U + slot * 24U + i]);
                recipes[slot].random_choices[i] = std::to_integer<std::uint8_t>(bytes[0xc8U + slot * 24U + i]);
                recipes[slot].processing_flags[i] = std::to_integer<std::uint8_t>(bytes[0x140U + slot * 24U + i]);
            }
        }
        result.working_program_marker = std::to_integer<std::uint8_t>(bytes[0x1bf]);
    }
    return result;
}

} // namespace axk
