#include "axklib/system_file.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "system_global_fields.hpp"
#include "system_remix_internal.hpp"

namespace axk {
namespace {

Error invalid(const std::string &message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message);
}

Result<void> apply_scalars(std::span<std::byte> bytes, const SystemGlobalParameters &patch, ASeriesModel model) {
    if (patch.total_eq[0].width_selection)
        return std::unexpected{invalid("Total EQ low boost has no width setting")};
    Result<void> result;
    detail::visit_system_global_scalars(
        patch, model == ASeriesModel::a5000,
        [&]<typename T>(std::size_t offset, const std::optional<T> &value, int minimum, int maximum, const char *name) {
            if (!result || !value)
                return;
            if (offset >= bytes.size() || (model == ASeriesModel::a4000 && offset >= 0x40U && offset < 0x50U)) {
                result =
                    std::unexpected{invalid(std::string{"Global "} + name + " is unavailable on the target model")};
            } else if (static_cast<int>(*value) < minimum || static_cast<int>(*value) > maximum) {
                result = std::unexpected{invalid(std::string{"Global "} + name + " is out of range")};
            } else {
                bytes[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(*value));
            }
        });
    return result;
}

void bit(std::byte &byte, const std::optional<bool> &value, unsigned mask) {
    if (value)
        byte = (byte & ~static_cast<std::byte>(mask)) | (*value ? static_cast<std::byte>(mask) : std::byte{});
}

Result<void> apply_flags(std::span<std::byte> bytes, const SystemGlobalParameters &p) {
    auto &flags = bytes[6];
    bit(flags, p.omni, 1);
    bit(flags, p.program_change_enabled, 2);
    bit(flags, p.audition_with_easy_edit, 32);
    bit(flags, p.audition_with_effects, 64);
    bit(flags, p.play_and_load, 128);
    if (p.wave_length_lock || p.wave_auto_zero || p.wave_auto_snap) {
        const auto count = static_cast<int>(p.wave_length_lock.value_or(false)) +
                           static_cast<int>(p.wave_auto_zero.value_or(false)) +
                           static_cast<int>(p.wave_auto_snap.value_or(false));
        if (count > 1)
            return std::unexpected{invalid("Wave length lock, auto zero and auto snap are mutually exclusive")};
        flags &= std::byte{0xe3};
        bit(flags, p.wave_length_lock, 4);
        bit(flags, p.wave_auto_zero, 8);
        bit(flags, p.wave_auto_snap, 16);
    }
    if (p.program_mode) {
        if (p.omni == true)
            return std::unexpected{invalid("A Program mode write clears Omni; explicit Omni enable conflicts")};
        flags &= std::byte{0xfe};
    }
    if (p.remix_auto_audition || p.knob_midi_out || p.function_key_midi_out) {
        if (bytes.size() <= 0x2fU)
            return std::unexpected{invalid("A3000 has no current Remix or MIDI-out flags")};
        auto &extra = bytes[0x2f];
        bit(extra, p.remix_auto_audition, 1);
        bit(extra, p.knob_midi_out, 2);
        bit(extra, p.function_key_midi_out, 4);
        if (std::to_integer<unsigned>(extra) > 1U)
            return std::unexpected{invalid("Edited Remix/MIDI-out byte would be normalized on load; MIDI-out flags and "
                                           "other retained bits must be zero")};
    }
    return {};
}

Result<void> apply_remix(std::span<std::byte> bytes, const SystemGlobalParameters &p) {
    if (!p.remix_type_selection && !p.remix_variation_selection)
        return {};
    const auto maximum_variation = bytes.size() == 0x2eU ? 3U : 7U;
    const auto maximum_type = bytes.size() == 0x2eU ? 4U : 7U;
    if (p.remix_type_selection && *p.remix_type_selection > maximum_type)
        return std::unexpected{invalid("Remix type is unavailable or would be normalized on load")};
    if (p.remix_variation_selection && *p.remix_variation_selection > maximum_variation)
        return std::unexpected{invalid("Remix variation is out of range")};
    auto &selection = bytes[0x24];
    if (p.remix_type_selection)
        selection = (selection & std::byte{15}) | static_cast<std::byte>(*p.remix_type_selection << 4U);
    if (p.remix_variation_selection)
        selection = (selection & std::byte{0xf0}) | static_cast<std::byte>(*p.remix_variation_selection);
    const auto raw = std::to_integer<unsigned>(selection);
    if ((raw >> 4U) > maximum_type || (raw & 15U) > maximum_variation)
        return std::unexpected{invalid("Remix selection requires a load-stable type and valid variation; supply both")};
    if ((raw >> 4U) >= 5U)
        return detail::validate_system_remix_selection(bytes, static_cast<std::uint8_t>((raw >> 4U) - 5U));
    return {};
}

Result<void> check_dependencies(std::span<const std::byte> bytes, const SystemGlobalParameters &p) {
    if (p.remix_zone_start || p.remix_zone_end) {
        const auto start = std::to_integer<unsigned>(bytes[0x1b8]);
        const auto end = std::to_integer<unsigned>(bytes[0x1b9]);
        if (start >= end || end > 8U)
            return std::unexpected{invalid("Remix zone must satisfy 0 <= start < end <= 8; supply both endpoints")};
    }
    for (std::size_t i = 0; i < p.assignable_output_level_offsets.size(); ++i) {
        if (p.assignable_output_level_offsets[i]) {
            const auto routing = std::to_integer<unsigned>(bytes[5]);
            if (routing > 5U || routing == i + 1U)
                return std::unexpected{
                    invalid("Assignable output level is unavailable for the duplicated output or undecoded routing")};
        }
    }
    return {};
}

} // namespace

Result<DecodedSystemFile> patch_system_global(const DecodedSystemFile &file, const SystemGlobalParameters &patch,
                                              ASeriesModel model) {
    const auto native = model == ASeriesModel::a3000;
    if ((model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000) ||
        file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Global target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    auto bytes = std::span{*encoded}.subspan(current_record_envelope_size + 0x30U, native ? 0x2eU : 0x1c0U);
    if (const auto applied = apply_scalars(bytes, patch, model); !applied)
        return std::unexpected{applied.error()};
    if (const auto applied = apply_flags(bytes, patch); !applied)
        return std::unexpected{applied.error()};
    if (const auto applied = apply_remix(bytes, patch); !applied)
        return std::unexpected{applied.error()};
    if (const auto checked = check_dependencies(bytes, patch); !checked)
        return std::unexpected{checked.error()};
    // Recompute the context projection through the canonical decoder.
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
