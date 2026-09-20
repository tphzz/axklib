#pragma once

#include <span>
#include <vector>

#include "axklib/error.hpp"

namespace axk::detail {
Result<std::vector<std::byte>> extend_sample_parameter_prefix(std::span<const std::byte> prefix);
} // namespace axk::detail
