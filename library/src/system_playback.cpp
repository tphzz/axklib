#include "axklib/system_file.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>

namespace axk {
namespace {

constexpr std::size_t sequence_offset = current_record_envelope_size + 0x64cU;
constexpr std::size_t digital_offset = current_record_envelope_size + 0x65cU;

Error invalid(const char *message) { return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message); }

} // namespace

Result<DecodedSystemPlayback> decode_system_playback(const DecodedSystemFile &file) {
    const auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Sequence port and digital-output settings require SYSTEM2")};
    DecodedSystemPlayback result;
    const auto bytes = std::span{*encoded};
    std::ranges::copy(bytes.subspan(sequence_offset, result.sequence_raw_bytes.size()),
                      result.sequence_raw_bytes.begin());
    std::ranges::copy(bytes.subspan(digital_offset, result.digital_output_raw_bytes.size()),
                      result.digital_output_raw_bytes.begin());
    const auto sequence = std::to_integer<unsigned>(bytes[sequence_offset]);
    const auto digital = std::to_integer<unsigned>(bytes[digital_offset]);
    if (sequence <= 1U)
        result.parameters.sequence_midi_port = sequence == 0U ? MidiPort::b : MidiPort::a;
    if (digital <= 1U)
        result.parameters.digital_output_bits =
            digital == 0U ? SystemDigitalOutputBits::bits20 : SystemDigitalOutputBits::bits24;
    return result;
}

Result<DecodedSystemFile> patch_system_playback(const DecodedSystemFile &file, const SystemPlaybackParameters &patch,
                                                ASeriesModel model) {
    if (model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Playback target model is unsupported")};
    const bool native = model == ASeriesModel::a3000;
    if (file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Playback target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (!patch.sequence_midi_port && !patch.digital_output_bits)
        return file;
    if (native)
        return std::unexpected{invalid("Sequence port and digital-output edits require SYSTEM2")};
    if (patch.sequence_midi_port && *patch.sequence_midi_port != MidiPort::a &&
        (*patch.sequence_midi_port != MidiPort::b || model != ASeriesModel::a5000))
        return std::unexpected{invalid("Sequence MIDI port is unavailable on the target model")};
    if (patch.digital_output_bits && *patch.digital_output_bits != SystemDigitalOutputBits::bits20 &&
        *patch.digital_output_bits != SystemDigitalOutputBits::bits24)
        return std::unexpected{invalid("Digital output bit depth must be 20 or 24")};
    if (patch.sequence_midi_port)
        (*encoded)[sequence_offset] = *patch.sequence_midi_port == MidiPort::a ? std::byte{1} : std::byte{};
    if (patch.digital_output_bits)
        (*encoded)[digital_offset] =
            *patch.digital_output_bits == SystemDigitalOutputBits::bits24 ? std::byte{1} : std::byte{};
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
