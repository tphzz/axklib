#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/conversion/plan.hpp"
#include "axklib/formats/tx16w/wave_writer.hpp"

namespace axk::conversion::tx16w_wave_disk {
[[nodiscard]] Result<tx16w::WaveEncoding> map_playback(const WaveTransfer &wave);
[[nodiscard]] std::int16_t quantize_pcm12(double value);
struct RenderedWave {
    std::vector<std::byte> bytes;
    bool rounded{};
};
[[nodiscard]] Result<RenderedWave> render_wave(const WaveTransfer &wave, const tx16w::WaveEncoding &encoding,
                                               const CancellationToken &cancel);
[[nodiscard]] std::string wave_filename(std::string_view source_name, std::size_t slot);
} // namespace axk::conversion::tx16w_wave_disk
