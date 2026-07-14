#include "audio_import_internal.hpp"

#include <cmath>
#include <string>

#include <soxr.h>

namespace axk::audio_import_detail {

Result<std::vector<double>> resample_vhq(std::span<const double> samples,
                                         std::size_t channels,
                                         std::uint32_t source_rate,
                                         std::uint32_t output_rate) {
    if (channels == 0U || samples.size() % channels != 0U) {
        return std::unexpected{make_error(
            ErrorCode::audio_unsupported_format, ErrorCategory::audio,
            "resampler input is not a complete channel frame")};
    }
    const auto input_frames = samples.size() / channels;
    const auto estimated_frames =
        static_cast<std::size_t>(std::ceil(static_cast<double>(input_frames) *
                                           output_rate / source_rate)) +
        16U;
    std::vector<double> converted(estimated_frames * channels);
    std::size_t input_done{};
    std::size_t output_done{};
    const auto io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
    const auto quality = soxr_quality_spec(SOXR_VHQ, 0);
    const auto error = soxr_oneshot(
        static_cast<double>(source_rate), static_cast<double>(output_rate),
        static_cast<unsigned>(channels), samples.data(), input_frames,
        &input_done, converted.data(), estimated_frames, &output_done, &io,
        &quality, nullptr);
    if (error != nullptr || input_done != input_frames) {
        return std::unexpected{make_error(
            ErrorCode::audio_unsupported_format, ErrorCategory::audio,
            std::string{"VHQ sample-rate conversion failed: "} +
                (error == nullptr ? "incomplete input" : error))};
    }
    converted.resize(output_done * channels);
    return converted;
}

} // namespace axk::audio_import_detail
