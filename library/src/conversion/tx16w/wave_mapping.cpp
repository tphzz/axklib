#include "axklib/tx16w_wave_mapping.hpp"

#include <algorithm>
#include <cmath>
#include <expected>
#include <utility>
#include <vector>

#include "axklib/resample.hpp"

namespace axk::conversion::tx16w_wave_disk {
Result<tx16w::WaveEncoding> map_playback(const WaveTransfer &wave) {
    const auto fail = [](const char *reason) -> Result<tx16w::WaveEncoding> {
        return std::unexpected(make_error(ErrorCode::unsupported_profile, ErrorCategory::audio, reason));
    };
    const bool looped = wave.loop_mode == WavePlayback::forward_loop;
    if (!wave.rate || (!looped && wave.loop_mode != WavePlayback::forward_one_shot))
        return fail("Only forward one-shot and forward loop playback can be written");
    // Of the proven encodings, 50 kHz is available only for looped Waves.
    const std::uint32_t rate = looped && wave.rate >= 41667U ? 50000U : 33333U;
    const auto scaled = [&](std::uint32_t frames) {
        return (static_cast<std::uint64_t>(frames) * rate + wave.rate / 2U) / wave.rate;
    };
    const auto frames = scaled(wave.frame_count);
    constexpr std::uint32_t minimum = 64U, maximum = 0x1ffffU;
    if (frames < 2U * minimum || frames > 2U * maximum)
        return fail(
            "TX16W attack and repeat each require 64 to 131071 frames; padding or truncation would change playback");
    auto attack = std::min(static_cast<std::uint32_t>(frames) - minimum, maximum);
    if (looped) {
        if (wave.loop_start < wave.first_frame || wave.loop_length == 0U ||
            static_cast<std::uint64_t>(wave.loop_start) + wave.loop_length !=
                static_cast<std::uint64_t>(wave.first_frame) + wave.frame_count)
            return fail("The TX16W repeat must end at the playback boundary; the source loop cannot be preserved");
        attack = static_cast<std::uint32_t>(scaled(wave.loop_start - wave.first_frame));
    }
    const auto repeat = static_cast<std::uint32_t>(frames) - attack;
    if (attack < minimum || attack > maximum || repeat < minimum || repeat > maximum)
        return fail("Mapped loop is outside the TX16W attack or repeat length range");
    return tx16w::WaveEncoding{rate, attack, repeat, looped};
}

std::int16_t quantize_pcm12(double value) {
    return static_cast<std::int16_t>(std::clamp(std::round(value / 16.0), -2048.0, 2047.0) * 16.0);
}

Result<RenderedWave> render_wave(const WaveTransfer &wave, const tx16w::WaveEncoding &encoding,
                                 const CancellationToken &cancel) {
    const auto frames = encoding.attack_frames + encoding.repeat_frames;
    auto samples = wave.read(0U, wave.frame_count, cancel);
    if (!samples)
        return std::unexpected(samples.error());
    bool rounded = false;
    if (wave.rate != encoding.sample_rate) {
        std::vector<double> input(samples->begin(), samples->end());
        auto result = audio::resample_vhq(input, 1, wave.rate, encoding.sample_rate);
        if (!result)
            return std::unexpected(result.error());
        if (result->size() != frames)
            return std::unexpected(make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                              "Resampling did not produce the reviewed frame count"));
        samples->resize(frames);
        for (std::size_t n = 0; n < frames; ++n) {
            (*samples)[n] = quantize_pcm12((*result)[n]);
            rounded |= static_cast<double>((*samples)[n]) != (*result)[n];
        }
    } else {
        for (auto &value : *samples) {
            const auto q = quantize_pcm12(value);
            rounded |= q != value;
            value = q;
        }
    }

    if (auto active = cancel.check(); !active)
        return std::unexpected(active.error());
    auto encoded = tx16w::encode_wave(encoding, *samples);
    if (!encoded)
        return std::unexpected(encoded.error());
    return RenderedWave{std::move(*encoded), rounded};
}

std::string wave_filename(std::string_view source_name, std::size_t slot) {
    std::string name;
    for (auto c : source_name) {
        if (name.size() == 8U)
            break;
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        name += ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    if (name.empty())
        name = "WAVE";
    return name + (slot < 10U ? ".W0" : ".W") + std::to_string(slot);
}
} // namespace axk::conversion::tx16w_wave_disk
