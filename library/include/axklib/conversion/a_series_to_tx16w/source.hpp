#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/conversion/plan.hpp"

namespace axk::conversion::a_series_to_tx16w {
struct WaveSource {
    std::string id;
    std::string name;
    std::uint32_t rate{};
    std::uint16_t width{};
    std::uint32_t first_frame{};
    std::uint32_t frame_count{};
    std::uint32_t loop_mode{};
    std::uint32_t loop_start{};
    std::uint32_t loop_length{};
    std::uint8_t root_key{};
    std::int8_t fine_tune{};
    std::shared_ptr<const native::SourceSet> sources;
    native::SourceLocation playback;

    [[nodiscard]] Result<std::vector<std::int16_t>> read(std::uint64_t first, std::size_t count,
                                                         const CancellationToken &cancellation = {}) const;
};

// Resolve every occurrence and playback window before materializing any audio.
[[nodiscard]] Result<std::vector<WaveSource>> select_waves(const Request &request, std::string_view document_id,
                                                           std::span<const std::string> ids,
                                                           const CancellationToken &cancellation = {});
} // namespace axk::conversion::a_series_to_tx16w
