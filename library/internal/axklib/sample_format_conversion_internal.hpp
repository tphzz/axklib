#pragma once

#include <span>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/sample_format_conversion.hpp"

namespace axk::detail {
Result<std::vector<std::byte>> extend_sample_parameter_prefix(std::span<const std::byte> prefix);
std::vector<std::byte> convert_sample_parameter_block(SampleFormatConversionPlan &plan,
                                                      std::span<const std::byte> block);
} // namespace axk::detail
