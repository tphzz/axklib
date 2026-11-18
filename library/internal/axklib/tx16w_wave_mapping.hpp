#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/conversion/a_series_to_tx16w/source.hpp"
#include "axklib/formats/tx16w/wave_writer.hpp"

namespace axk::conversion::a_series_to_tx16w {
[[nodiscard]] Result<tx16w::WaveEncoding> map_playback(const WaveSource &wave);
[[nodiscard]] std::int16_t quantize_pcm12(double value);
struct RenderedWave {
    std::vector<std::byte> bytes;
    bool rounded{};
};
[[nodiscard]] Result<RenderedWave> render_wave(const WaveSource &wave, const tx16w::WaveEncoding &encoding,
                                               const CancellationToken &cancel);
[[nodiscard]] std::string wave_filename(std::string_view source_name, std::size_t slot);
} // namespace axk::conversion::a_series_to_tx16w
