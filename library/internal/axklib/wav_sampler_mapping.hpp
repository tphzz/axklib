#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "axklib/wav_stream.hpp"

namespace axk::audio_internal {

struct ASeriesSamplerParameters {
    std::uint32_t sample_rate{};
    std::uint64_t frame_count{};
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
    std::uint8_t key_low{};
    std::uint8_t key_high{127U};
    std::uint8_t velocity_low{};
    std::uint8_t velocity_high{127U};
    std::uint8_t loop_mode{};
    std::uint64_t loop_start{};
    std::uint64_t loop_length{};
    std::string context{"A-series audio"};
};

struct WavSamplerMapping {
    WavSamplerChunks chunks;
    std::vector<PublicationWarning> warnings;
};

[[nodiscard]] WavSamplerMapping map_a_series_sampler_metadata(const ASeriesSamplerParameters &parameters);
[[nodiscard]] ASeriesSamplerParameters waveform_sampler_parameters(const Waveform &waveform);
void apply_sampler_mapping(WavSource &source, WavSamplerMapping mapping);

} // namespace axk::audio_internal
