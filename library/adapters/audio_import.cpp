#include "axklib/writer.hpp"

#include "audio_import_internal.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>

namespace axk {
namespace {

Error audio_import_error(std::string message) {
    return make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio, std::move(message));
}

Error wave_data_too_large_error(std::uint64_t bytes_per_channel) {
    constexpr double bytes_per_mib = 1024.0 * 1024.0;
    return make_error(
        ErrorCode::audio_wave_data_too_large, ErrorCategory::audio,
        std::format("Converted Wave Data is {:.1f} MiB per channel ({} bytes); A-series hardware supports at most "
                    "32 MiB per channel ({} bytes)",
                    static_cast<double>(bytes_per_channel) / bytes_per_mib, bytes_per_channel,
                    maximum_wave_data_pcm16_bytes_per_channel));
}

} // namespace

Result<audio_import_detail::ProjectedAudioSize>
audio_import_detail::project_sampler_audio_size(std::uint64_t source_frames, std::size_t channels,
                                                std::uint32_t source_rate, std::uint32_t output_rate,
                                                std::uint8_t output_sample_width_bytes) {
    if ((channels != 1U && channels != 2U) || source_rate == 0U || output_rate == 0U ||
        (output_sample_width_bytes != 1U && output_sample_width_bytes != 2U)) {
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::audio,
                                          "audio size projection requires mono or stereo audio and valid rates")};
    }
    const auto whole = source_frames / source_rate;
    const auto remainder = source_frames % source_rate;
    if (whole > std::numeric_limits<std::uint64_t>::max() / output_rate) {
        return std::unexpected{
            make_error(ErrorCode::integer_overflow, ErrorCategory::audio, "projected Wave Data size overflows")};
    }
    const auto whole_output = whole * output_rate;
    const auto partial_numerator = remainder * output_rate;
    const auto partial_output = partial_numerator / source_rate + (partial_numerator % source_rate == 0U ? 0U : 1U);
    if (whole_output > std::numeric_limits<std::uint64_t>::max() - partial_output) {
        return std::unexpected{
            make_error(ErrorCode::integer_overflow, ErrorCategory::audio, "projected Wave Data size overflows")};
    }
    const auto output_frames = whole_output + partial_output;
    if (output_frames > std::numeric_limits<std::uint64_t>::max() / output_sample_width_bytes) {
        return std::unexpected{
            make_error(ErrorCode::integer_overflow, ErrorCategory::audio, "projected Wave Data size overflows")};
    }
    const auto bytes_per_channel = output_frames * output_sample_width_bytes;
    if (bytes_per_channel > std::numeric_limits<std::uint64_t>::max() / channels) {
        return std::unexpected{
            make_error(ErrorCode::integer_overflow, ErrorCategory::audio, "projected Wave Data size overflows")};
    }
    return ProjectedAudioSize{
        .output_frames = output_frames,
        .bytes_per_channel = bytes_per_channel,
        .total_bytes = bytes_per_channel * channels,
        .valid = output_frames <= maximum_wave_data_frames_per_channel,
    };
}

Result<std::uint32_t> choose_sampler_sample_rate(std::uint32_t source_rate,
                                                 std::optional<std::uint32_t> target_sample_rate) {
    if (target_sample_rate) {
        if (!std::ranges::contains(supported_sampler_sample_rates, *target_sample_rate)) {
            return std::unexpected{audio_import_error("target sample rate is not supported by A-series hardware")};
        }
        return *target_sample_rate;
    }
    return std::ranges::contains(supported_sampler_sample_rates, source_rate) ? source_rate
                                                                              : default_sampler_sample_rate;
}

Result<AudioSourceInfo> inspect_sampler_audio(const std::filesystem::path &path,
                                              std::optional<std::uint32_t> target_sample_rate) {
    const auto source = audio_import_detail::inspect_sndfile(path);
    if (!source)
        return std::unexpected{source.error()};
    const auto output_rate = choose_sampler_sample_rate(source->sample_rate, target_sample_rate);
    if (!output_rate)
        return std::unexpected{output_rate.error()};
    const auto projected = audio_import_detail::project_sampler_audio_size(source->frames, source->channels,
                                                                           source->sample_rate, *output_rate);
    if (!projected)
        return std::unexpected{projected.error()};
    const bool resampled = *output_rate != source->sample_rate;
    const bool dither = resampled || source->reduces_precision;
    AudioSourceInfo result{
        .source_format = source->format,
        .source_subtype = source->subtype,
        .channels = static_cast<std::uint8_t>(source->channels),
        .frame_count = source->frames,
        .source_sample_rate = source->sample_rate,
        .output_sample_rate = *output_rate,
        .source_sample_width_bits = source->sample_width_bits,
        .output_sample_width_bits = sampler_output_sample_width_bits,
        .resampled = resampled,
        .quantized = dither,
        .sample_width_converted = source->sample_width_bits != 16U,
        .dither_algorithm = dither ? std::string{audio_import_detail::dither_algorithm} : std::string{},
        .projected_output_frame_count = projected->output_frames,
        .projected_output_bytes_per_channel = projected->bytes_per_channel,
        .projected_output_bytes_total = projected->total_bytes,
        .maximum_output_bytes_per_channel = maximum_wave_data_pcm16_bytes_per_channel,
        .valid = projected->valid,
        .issues = {},
    };
    if (!projected->valid) {
        const auto error = wave_data_too_large_error(projected->bytes_per_channel);
        result.issues.push_back({"wave_data_channel_too_large", error.message, true});
    }
    return result;
}

Result<ImportedAudio> import_sampler_audio(const std::filesystem::path &path, const AudioImportOptions &options) {
    if (options.expected_channels != 1U && options.expected_channels != 2U) {
        return std::unexpected{audio_import_error("expected channel count must be one or two")};
    }
    const auto source = audio_import_detail::inspect_sndfile(path, options.expected_channels);
    if (!source)
        return std::unexpected{source.error()};
    const auto output_rate = choose_sampler_sample_rate(source->sample_rate, options.target_sample_rate);
    if (!output_rate)
        return std::unexpected{output_rate.error()};
    const auto projected = audio_import_detail::project_sampler_audio_size(source->frames, source->channels,
                                                                           source->sample_rate, *output_rate);
    if (!projected)
        return std::unexpected{projected.error()};
    if (!projected->valid)
        return std::unexpected{wave_data_too_large_error(projected->bytes_per_channel)};
    const bool resampled = *output_rate != source->sample_rate;
    const bool exact_integer_conversion = (source->is_pcm8 || source->is_pcm16) && !resampled;
    const bool dither = resampled || source->reduces_precision;
    std::vector<std::int16_t> quantized;
    std::uint64_t clipped{};
    if (exact_integer_conversion) {
        auto decoded = audio_import_detail::decode_sndfile_pcm16(path, *source);
        if (!decoded)
            return std::unexpected{decoded.error()};
        quantized = std::move(*decoded);
    } else {
        auto decoded = audio_import_detail::decode_sndfile_float64(path, *source);
        if (!decoded)
            return std::unexpected{decoded.error()};
        auto floating = std::move(*decoded);
        if (resampled) {
            auto converted =
                audio_import_detail::resample_vhq(floating, source->channels, source->sample_rate, *output_rate);
            if (!converted)
                return std::unexpected{converted.error()};
            floating = std::move(*converted);
        }
        auto converted = audio_import_detail::quantize_pcm16(floating, dither);
        if (!converted)
            return std::unexpected{converted.error()};
        quantized = std::move(converted->samples);
        clipped = converted->clipped_samples;
    }
    const auto output_frames = quantized.size() / source->channels;
    if (output_frames > maximum_wave_data_frames_per_channel) {
        return std::unexpected{wave_data_too_large_error(output_frames * sizeof(std::int16_t))};
    }
    ImportedAudio result;
    result.source_path = path;
    result.source_format = source->format;
    result.source_subtype = source->subtype;
    result.source_channels = static_cast<std::uint8_t>(source->channels);
    result.source_sample_rate = source->sample_rate;
    result.output_sample_rate = *output_rate;
    result.source_sample_width_bits = source->sample_width_bits;
    result.output_sample_width_bits = sampler_output_sample_width_bits;
    result.output_frames = output_frames;
    result.pcm_channels = audio_import_detail::split_pcm16(quantized, source->channels);
    result.resampled = resampled;
    result.quantized = !exact_integer_conversion;
    result.sample_width_converted = source->sample_width_bits != 16U;
    if (dither)
        result.dither_algorithm = std::string{audio_import_detail::dither_algorithm};
    result.clipped_samples = clipped;
    return result;
}

} // namespace axk
