#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "axklib/sample_parameters.hpp"

namespace axk::detail {

enum class SampleParameterLayout : std::uint8_t { current, current_prefix_only, a3000 };

bool has_sample_parameter_values(const SampleParameters &parameters);
Result<void> validate_sample_parameters(const SampleParameters &parameters,
                                        SampleParameterGeneration generation = SampleParameterGeneration::current);
// Native blocks contain 188 bytes. Current workspaces contain 224 bytes;
// prefix-only current objects do not have a canonical controller tail.
Result<void> apply_sample_parameters_to_block(std::span<std::byte> block, const SampleParameters &parameters,
                                              SampleParameterLayout layout = SampleParameterLayout::current);
void merge_sample_parameters(SampleParameters &destination, const SampleParameters &source);

} // namespace axk::detail
