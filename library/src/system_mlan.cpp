#include "axklib/system_file.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

namespace axk {
namespace {

constexpr std::size_t mlan_body_offset = 0x664U;
constexpr std::size_t system_header_size = 0x20U;

Error invalid(const char *message) { return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message); }

} // namespace

Result<DecodedSystemMlan> decode_system_mlan(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a4000_a5000_system2 || file.storage_revision == 0U)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "mLAN routing requires revision-1 SYSTEM2")};
    if (file.storage_revision != 1U || file.system_bulk_bytes.size() != 0xfe0U)
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::object, "mLAN System File layout is invalid")};
    const auto bytes = std::span{file.system_bulk_bytes}.subspan(mlan_body_offset - system_header_size, 16U);
    DecodedSystemMlan result;
    std::ranges::copy(bytes, result.raw_bytes.begin());
    const auto midi = std::to_integer<unsigned>(bytes[0]);
    const auto audio = std::to_integer<unsigned>(bytes[1]);
    if (midi <= 2U)
        result.parameters.midi_input = static_cast<SystemMlanMidiInput>(midi);
    if (audio <= 1U)
        result.parameters.audio_input = static_cast<SystemMlanAudioInput>(audio);
    return result;
}

Result<DecodedSystemFile> patch_system_mlan(const DecodedSystemFile &file, const SystemMlanParameters &patch,
                                            ASeriesModel model) {
    if (model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{
            make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, "mLAN target model is unsupported")};
    const bool native = model == ASeriesModel::a3000;
    if (file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "mLAN target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (!patch.midi_input && !patch.audio_input)
        return file;
    if (native || file.storage_revision != 1U)
        return std::unexpected{invalid("mLAN routing edits require revision-1 SYSTEM2")};
    if (patch.midi_input && static_cast<unsigned>(*patch.midi_input) > (model == ASeriesModel::a5000 ? 2U : 1U))
        return std::unexpected{invalid("mLAN MIDI input is unavailable on the target model")};
    if (patch.audio_input && static_cast<unsigned>(*patch.audio_input) > 1U)
        return std::unexpected{invalid("mLAN audio input is invalid")};
    const auto offset = current_record_envelope_size + mlan_body_offset;
    if (patch.midi_input)
        (*encoded)[offset] = static_cast<std::byte>(*patch.midi_input);
    if (patch.audio_input)
        (*encoded)[offset + 1U] = static_cast<std::byte>(*patch.audio_input);
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
