#pragma once

#include <cstdint>
#include <span>

#include "axklib/writer.hpp"

namespace axk {

// Samples are already expanded to full-scale signed PCM16, one value per frame.
[[nodiscard]] Result<ImportedAudio> import_mono_pcm16(std::span<const std::int16_t> frames, std::uint32_t source_rate,
                                                      std::uint32_t target_rate);

} // namespace axk
