#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/conversion/plan.hpp"

namespace axk::conversion::a_series_waves {
[[nodiscard]] WaveReaderAdapter reader();

// Resolve every occurrence and playback window before materializing any audio.
[[nodiscard]] Result<std::vector<WaveTransfer>> select_waves(const Request &request, std::string_view document_id,
                                                             std::span<const std::string> ids,
                                                             const CancellationToken &cancellation = {});
} // namespace axk::conversion::a_series_waves
