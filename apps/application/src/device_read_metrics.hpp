#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "axklib/io.hpp"

namespace axk::app::device_detail {

struct ReadCounters {
    std::uint64_t calls{};
    std::uint64_t bytes{};
    std::uint64_t nanoseconds{};
};

nlohmann::json device_read_metrics(const RandomAccessReader *reader);

} // namespace axk::app::device_detail
