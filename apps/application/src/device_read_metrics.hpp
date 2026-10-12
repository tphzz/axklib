#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "axklib/io.hpp"

namespace axk::app::device_detail {

struct ReadCounters {
    std::uint64_t calls{};
    std::uint64_t bytes{};
    std::uint64_t nanoseconds{};
    std::uint64_t verification_passes{};
    std::uint64_t verification_reads{};
    std::uint64_t verified_bytes{};
    std::uint64_t verification_gap_bytes{};
};

nlohmann::json device_read_metrics(const RandomAccessReader *reader);

} // namespace axk::app::device_detail
