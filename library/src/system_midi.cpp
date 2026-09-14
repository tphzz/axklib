#include "axklib/system_file.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace axk {
namespace {

std::size_t midi_offset(SystemFileKind kind) {
    return current_record_envelope_size + (kind == SystemFileKind::a3000_system ? 0x1d8U : 0x3fcU);
}

std::optional<bool> boolean(std::byte value) {
    const auto raw = std::to_integer<unsigned>(value);
    return raw <= 1U ? std::optional<bool>{raw != 0U} : std::nullopt;
}

Error invalid(const char *message) { return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message); }

} // namespace

Result<DecodedSystemMidi> decode_system_midi(const DecodedSystemFile &file) {
    const auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    const bool native = file.kind == SystemFileKind::a3000_system;
    const auto bytes = std::span{*encoded}.subspan(midi_offset(file.kind), native ? 8U : 9U);
    DecodedSystemMidi result;
    result.raw_bytes.assign(bytes.begin(), bytes.end());
    auto &p = result.parameters;
    p.bulk_protect = boolean(bytes[2]);
    p.aftertouch_disabled = boolean(bytes[3]);
    p.control_change_disabled = boolean(bytes[4]);
    p.pitch_bend_disabled = boolean(bytes[5]);
    if (std::to_integer<unsigned>(bytes[7]) <= 17U)
        p.device_number = std::to_integer<std::uint8_t>(bytes[7]);
    if (!native && std::to_integer<unsigned>(bytes[8]) <= 1U)
        p.sysex_receive_port = static_cast<SystemSysexReceivePort>(bytes[8]);
    return result;
}

Result<DecodedSystemFile> patch_system_midi(const DecodedSystemFile &file, const SystemMidiParameters &patch,
                                            ASeriesModel model) {
    if (model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{
            make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, "MIDI target model is unsupported")};
    const bool native = model == ASeriesModel::a3000;
    if (file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "MIDI target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (patch.device_number && *patch.device_number > 17U)
        return std::unexpected{invalid("MIDI device number must be off, 1..16 or all")};
    if (patch.sysex_receive_port &&
        (native || static_cast<unsigned>(*patch.sysex_receive_port) > (model == ASeriesModel::a5000 ? 1U : 0U)))
        return std::unexpected{invalid("SysEx receive port is unavailable on the target model")};
    const auto offset = midi_offset(file.kind);
    if (patch.bulk_protect)
        (*encoded)[offset + 2U] = static_cast<std::byte>(*patch.bulk_protect);
    if (patch.aftertouch_disabled)
        (*encoded)[offset + 3U] = static_cast<std::byte>(*patch.aftertouch_disabled);
    if (patch.control_change_disabled)
        (*encoded)[offset + 4U] = static_cast<std::byte>(*patch.control_change_disabled);
    if (patch.pitch_bend_disabled)
        (*encoded)[offset + 5U] = static_cast<std::byte>(*patch.pitch_bend_disabled);
    if (patch.device_number)
        (*encoded)[offset + 7U] = static_cast<std::byte>(*patch.device_number);
    if (patch.sysex_receive_port)
        (*encoded)[offset + 8U] = static_cast<std::byte>(*patch.sysex_receive_port);
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
