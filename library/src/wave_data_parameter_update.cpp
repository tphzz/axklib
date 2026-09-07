#include "axklib/writer_internal.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "alteration_manifest_wave_data.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/object.hpp"

namespace axk::detail {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}
} // namespace

Result<void> validate_wave_data_parameters(const WaveDataParameters &value) {
    if (!value.root_key && !value.fine_tune_cents && !value.loop_mode && !value.wave_start_frame &&
        !value.wave_length_frames && !value.loop_start_frame && !value.loop_length_frames)
        return std::unexpected{invalid("Wave Data parameters must not be empty")};
    if ((value.root_key && *value.root_key > 127U) ||
        (value.fine_tune_cents && (*value.fine_tune_cents < -63 || *value.fine_tune_cents > 63)) ||
        (value.loop_mode && static_cast<std::uint8_t>(*value.loop_mode) > 5U))
        return std::unexpected{invalid("Wave Data parameter is outside its supported range")};
    return {};
}

Result<WaveDataParameters> parse_wave_data_parameters_json(const nlohmann::json &value) {
    if (!value.is_object())
        return std::unexpected{invalid("Wave Data parameters must be an object")};
    WaveDataParameters result;
    for (const auto &[name, raw] : value.items()) {
        if (!raw.is_number_integer())
            return std::unexpected{invalid("Wave Data parameter must be an integer: " + name)};
        // Compare JSON numbers before narrowing, including unsigned values above INT64_MAX.
        const auto minimum = name == "fine_tune_cents" ? -63 : 0;
        const auto maximum = name == "root_key"          ? 127ULL
                             : name == "fine_tune_cents" ? 63ULL
                             : name == "loop_mode"
                                 ? 5ULL
                                 : static_cast<unsigned long long>(std::numeric_limits<std::uint32_t>::max());
        if (raw.is_number_unsigned() && raw.get<std::uint64_t>() > maximum)
            return std::unexpected{invalid("Wave Data parameter is outside its supported range: " + name)};
        const auto number = raw.get<std::int64_t>();
        if (number < minimum || number > static_cast<std::int64_t>(maximum))
            return std::unexpected{invalid("Wave Data parameter is outside its supported range: " + name)};
        if (name == "root_key")
            result.root_key = raw.get<std::uint8_t>();
        else if (name == "fine_tune_cents")
            result.fine_tune_cents = raw.get<std::int8_t>();
        else if (name == "loop_mode")
            result.loop_mode = static_cast<AudioSamplerLoopMode>(raw.get<std::uint8_t>());
        else if (name == "wave_start_frame")
            result.wave_start_frame = raw.get<std::uint32_t>();
        else if (name == "wave_length_frames")
            result.wave_length_frames = raw.get<std::uint32_t>();
        else if (name == "loop_start_frame")
            result.loop_start_frame = raw.get<std::uint32_t>();
        else if (name == "loop_length_frames")
            result.loop_length_frames = raw.get<std::uint32_t>();
        else
            return std::unexpected{invalid("Unknown Wave Data parameter: " + name)};
    }
    if (auto valid = validate_wave_data_parameters(result); !valid)
        return std::unexpected{valid.error()};
    return result;
}

Result<void> apply_wave_data_parameters_to_payload(std::vector<std::byte> &payload,
                                                   const WaveDataParameters &parameters) {
    if (auto valid = validate_wave_data_parameters(parameters); !valid)
        return valid;
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *wave = std::get_if<CurrentSmpl>(&decoded->payload);
    if (!wave)
        return std::unexpected{invalid("Object is not current Wave Data")};
    if (auto valid = validate_smpl_pcm_transfer_control(*wave); !valid)
        return valid;
    const auto width = wave->stored_sample_width_bytes.value;
    if ((width != 1U && width != 2U) || wave->stored_segment_offset != 0U ||
        wave->stored_segment_bytes != wave->stored_pcm_bytes || wave->stored_pcm_offset < 0xacU ||
        wave->stored_pcm_offset > payload.size() || wave->stored_pcm_bytes > payload.size() - wave->stored_pcm_offset ||
        wave->stored_pcm_bytes % width != 0U)
        return std::unexpected{invalid("Wave Data update requires a complete supported PCM payload")};
    const auto root = parameters.root_key.value_or(wave->root_key.value);
    const auto fine = parameters.fine_tune_cents.value_or(wave->fine_tune_cents.value);
    const auto changes_pitch = parameters.root_key || parameters.fine_tune_cents;
    if (changes_pitch && (root > 127U || fine < -63 || fine > 63 || wave->sample_rate.value == 0U ||
                          wave->sample_rate.value != wave->duplicate_sample_rate.value))
        return std::unexpected{invalid("Wave Data pitch metadata is inconsistent")};
    const auto changes_window = parameters.loop_mode || parameters.wave_start_frame || parameters.wave_length_frames ||
                                parameters.loop_start_frame || parameters.loop_length_frames;
    if (changes_window) {
        const auto mode = parameters.loop_mode.value_or(static_cast<AudioSamplerLoopMode>(wave->loop_mode.value));
        const std::uint64_t start = parameters.wave_start_frame.value_or(wave->wave_start_frame.value);
        const std::uint64_t length = parameters.wave_length_frames.value_or(wave->wave_length_frames.value);
        const std::uint64_t loop_start = parameters.loop_start_frame.value_or(wave->loop_start_frame.value);
        const std::uint64_t loop_length = parameters.loop_length_frames.value_or(wave->loop_length_frames.value);
        const auto stored_frames = wave->stored_pcm_bytes / width;
        const auto repeating =
            mode == AudioSamplerLoopMode::forward_loop || mode == AudioSamplerLoopMode::forward_loop_release;
        if (static_cast<std::uint8_t>(mode) > 5U || length == 0U || start + length > stored_frames ||
            start + length > maximum_wave_data_frames_per_channel ||
            (loop_length == 0U ? (repeating || loop_start != 0U)
                               : (loop_start < start || loop_start + loop_length > start + length)))
            return std::unexpected{invalid("Wave Data playback or loop window is outside its stored frames")};
    }
    auto updated = payload;
    ByteWriter writer{updated};
    if (parameters.root_key)
        updated[0x7eU] = static_cast<std::byte>(root);
    if (parameters.fine_tune_cents)
        updated[0x7fU] = static_cast<std::byte>(static_cast<std::uint8_t>(fine));
    if (changes_pitch) {
        if (auto written = writer.write_be16(0x80U, sample_pitch_word(root, fine, wave->sample_rate.value)); !written)
            return std::unexpected{written.error()};
    }
    if (parameters.loop_mode)
        updated[0x85U] = static_cast<std::byte>(*parameters.loop_mode);
    for (const auto &[offset, value] :
         {std::pair{0x8eU, parameters.wave_start_frame}, std::pair{0x92U, parameters.wave_length_frames},
          std::pair{0x96U, parameters.loop_start_frame}, std::pair{0x9aU, parameters.loop_length_frames}}) {
        if (value) {
            if (auto written = writer.write_be32(offset, *value); !written)
                return std::unexpected{written.error()};
        }
    }
    payload = std::move(updated);
    return {};
}

} // namespace axk::detail
