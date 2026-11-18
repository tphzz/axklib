#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "axklib/error.hpp"

namespace axk::audio {
[[nodiscard]] Result<std::vector<double>> resample_vhq(std::span<const double> samples, std::size_t channels,
                                                       std::uint32_t source_rate, std::uint32_t output_rate);
}
