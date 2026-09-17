#include "axklib/system_file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "axklib/bytes.hpp"
#include "axklib/program_parameter_codec.hpp"
#include "system_recording_fields.hpp"

namespace axk {
namespace {

SystemRecordingParameters decode_configuration(std::span<const std::byte> bytes, bool native) {
    const ByteReader reader{bytes};
    SystemRecordingParameters result;
    detail::visit_system_recording_fields(native, [&]<typename T>(std::size_t offset,
                                                                  std::optional<T> SystemRecordingParameters::*member,
                                                                  int minimum, int maximum, const char *) {
        int raw;
        if constexpr (std::is_same_v<T, std::uint16_t>)
            raw = *reader.be16(offset);
        else if constexpr (std::is_same_v<T, std::int8_t>)
            raw = *reader.s8(offset);
        else
            raw = *reader.u8(offset);
        if (raw >= minimum && raw <= maximum)
            result.*member = static_cast<T>(raw);
    });
    return result;
}

} // namespace

Result<DecodedSystemRecording> decode_system_recording(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a3000_system && file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Recording System File kind is unsupported")};
    const auto native = file.kind == SystemFileKind::a3000_system;
    if (file.storage_revision > (native ? 0U : 1U) || file.system_bulk_bytes.size() != (native ? 0x348U : 0xfe0U))
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::object, "Recording System File layout is invalid")};
    const auto raw = std::span{file.system_bulk_bytes}.subspan(native ? 0x110U : 0x320U, native ? 154U : 176U);
    DecodedSystemRecording result;
    result.raw_bytes.assign(raw.begin(), raw.end());
    result.parameters = decode_configuration(raw.subspan(120U), native);
    for (std::size_t slot = 0; slot < result.effects.size(); ++slot) {
        const auto bytes = raw.subspan(slot * 40U, 40U);
        const ByteReader reader{bytes};
        ProgEffectBlock effect;
        std::ranges::copy(bytes, effect.raw_bytes.begin());
        effect.type = *reader.u8(native ? 7U : 6U);
        for (std::size_t parameter = 0; parameter < effect.parameter_values.size(); ++parameter)
            effect.parameter_values[parameter] = *reader.be16(8U + parameter * 2U);
        result.effects[slot] = detail::decode_program_effect_parameters(
            effect, slot,
            native ? detail::ProgramParameterGeneration::a3000 : detail::ProgramParameterGeneration::current);
    }
    return result;
}

} // namespace axk
