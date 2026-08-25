#include "axklib/wav_sampler_mapping.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace axk::audio_internal {
namespace {

void warn(WavSamplerMapping &mapping, std::string code, std::string message) {
    mapping.warnings.push_back({std::move(code), std::move(message)});
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> smpl_pitch(const ASeriesSamplerParameters &parameters) {
    const auto total_cents = static_cast<std::int32_t>(parameters.root_key) * 100 + parameters.fine_tune_cents;
    if (total_cents < 0)
        return std::nullopt;
    const auto unity_note = static_cast<std::uint32_t>(total_cents / 100);
    const auto remainder = static_cast<std::uint32_t>(total_cents % 100);
    if (unity_note > 127U)
        return std::nullopt;
    constexpr auto fraction_scale = 4'294'967'296.0;
    const auto fraction =
        static_cast<std::uint32_t>(std::llround(static_cast<double>(remainder) * fraction_scale / 100.0));
    return std::pair{unity_note, fraction};
}

std::optional<std::pair<std::uint8_t, std::int8_t>> inst_pitch(const ASeriesSamplerParameters &parameters) {
    auto root_key = static_cast<std::int32_t>(parameters.root_key);
    auto fine_tune = static_cast<std::int32_t>(parameters.fine_tune_cents);
    while (fine_tune > 50 && root_key < 127) {
        ++root_key;
        fine_tune -= 100;
    }
    while (fine_tune < -50 && root_key > 0) {
        --root_key;
        fine_tune += 100;
    }
    if (root_key < 0 || root_key > 127 || fine_tune < -50 || fine_tune > 50)
        return std::nullopt;
    return std::pair{static_cast<std::uint8_t>(root_key), static_cast<std::int8_t>(fine_tune)};
}

void map_loop(const ASeriesSamplerParameters &parameters, WavSamplerMapping &mapping) {
    if (parameters.loop_mode == 0U || parameters.loop_mode == 4U)
        return;
    if (parameters.loop_mode == 3U || parameters.loop_mode == 5U) {
        warn(mapping, "wav_sampler_reverse_playback_omitted",
             parameters.context +
                 " uses reverse playback, which standard WAV sampler loops cannot represent; no smpl loop was written");
        return;
    }
    if (parameters.loop_mode != 1U && parameters.loop_mode != 2U) {
        warn(mapping, "wav_sampler_loop_mode_omitted",
             parameters.context + " has an unknown loop mode; no smpl loop was written");
        return;
    }
    if (parameters.loop_length == 0U || parameters.loop_start >= parameters.frame_count ||
        parameters.loop_length > parameters.frame_count - parameters.loop_start ||
        parameters.loop_start > std::numeric_limits<std::uint32_t>::max() ||
        parameters.loop_length - 1U > std::numeric_limits<std::uint32_t>::max() - parameters.loop_start) {
        warn(mapping, "wav_sampler_loop_bounds_omitted",
             parameters.context + " has invalid or unrepresentable loop bounds; no smpl loop was written");
        return;
    }
    mapping.chunks.smpl->loops.push_back({
        .identifier = 0U,
        .type = 0U,
        .start = static_cast<std::uint32_t>(parameters.loop_start),
        .inclusive_end = static_cast<std::uint32_t>(parameters.loop_start + parameters.loop_length - 1U),
        .fraction = 0U,
        .play_count = 0U,
    });
    if (parameters.loop_mode == 2U) {
        warn(mapping, "wav_sampler_release_tail_approximated",
             parameters.context + " loops until key release on the A-series; WAV preserves the loop bounds as an "
                                  "indefinite forward loop but cannot preserve the release-tail behavior");
    }
}

} // namespace

WavSamplerMapping map_a_series_sampler_metadata(const ASeriesSamplerParameters &parameters) {
    WavSamplerMapping result;
    if (parameters.sample_rate != 0U) {
        const auto period = 1'000'000'000ULL / parameters.sample_rate;
        const auto pitch = smpl_pitch(parameters);
        if (period <= std::numeric_limits<std::uint32_t>::max() && pitch) {
            result.chunks.smpl = WavSmplChunk{
                .manufacturer = 0U,
                .product = 0U,
                .sample_period_nanoseconds = static_cast<std::uint32_t>(period),
                .midi_unity_note = pitch->first,
                .midi_pitch_fraction = pitch->second,
                .smpte_format = 0U,
                .smpte_offset = 0U,
                .loops = {},
                .sampler_specific_data = {},
            };
            map_loop(parameters, result);
        } else {
            warn(result, "wav_smpl_metadata_omitted",
                 parameters.context + " has pitch or rate metadata that cannot be represented in a WAV smpl chunk");
        }
    } else {
        warn(result, "wav_smpl_metadata_omitted",
             parameters.context + " has no sample rate, so WAV smpl metadata was omitted");
    }

    const auto pitch = inst_pitch(parameters);
    const auto ranges_valid = parameters.key_low <= parameters.key_high && parameters.key_high <= 127U &&
                              parameters.velocity_low <= parameters.velocity_high && parameters.velocity_high <= 127U;
    if (pitch && ranges_valid) {
        result.chunks.inst = WavInstChunk{
            .root_key = pitch->first,
            .fine_tune_cents = pitch->second,
            .gain_decibels = 0,
            .key_low = parameters.key_low,
            .key_high = parameters.key_high,
            .velocity_low = parameters.velocity_low,
            .velocity_high = parameters.velocity_high,
        };
    } else {
        warn(result, "wav_instrument_metadata_omitted",
             parameters.context + " has pitch, key, or velocity metadata outside the WAV inst limits");
    }
    return result;
}

ASeriesSamplerParameters waveform_sampler_parameters(const Waveform &waveform) {
    return {
        .sample_rate = waveform.format.sample_rate,
        .frame_count = waveform.frame_count,
        .root_key = waveform.root_key,
        .fine_tune_cents = waveform.fine_tune_cents,
        .key_low = 0U,
        .key_high = 127U,
        .velocity_low = 0U,
        .velocity_high = 127U,
        .loop_mode = waveform.loop_mode,
        .loop_start = waveform.loop_start,
        .loop_length = waveform.loop_length,
        .context = "Wave Data " + waveform.name,
    };
}

void apply_sampler_mapping(WavSource &source, WavSamplerMapping mapping) {
    source.sampler = std::move(mapping.chunks);
    source.warnings.insert(source.warnings.end(), std::make_move_iterator(mapping.warnings.begin()),
                           std::make_move_iterator(mapping.warnings.end()));
}

} // namespace axk::audio_internal
