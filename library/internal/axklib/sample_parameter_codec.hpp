#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "axklib/sample_parameters.hpp"
#include "axklib/sample_storage.hpp"

namespace axk::detail {

enum class SampleParameterLayout : std::uint8_t { a4000_a5000, a3000 };

bool has_sample_parameter_values(const SampleParameters &parameters);
Result<void> validate_sample_parameter_patch(const SampleParameters &parameters);
// Validate only supplied values and dependencies that are fully specified.
Result<void>
validate_sample_parameter_fields(const SampleParameters &parameters,
                                 SampleParameterGeneration generation = SampleParameterGeneration::a4000_a5000);
// Validate fresh authoring parameters with the creation defaults applied.
Result<void> validate_sample_parameters(const SampleParameters &parameters,
                                        SampleParameterGeneration generation = SampleParameterGeneration::a4000_a5000);
Result<void> validate_sample_authoring_parameters(const SampleParameters &parameters, SampleStorageFormat format);
// Native blocks contain 188 bytes. Current workspaces contain 224 bytes;
// prefix-only current objects do not have a canonical controller tail.
Result<void> apply_sample_parameters_to_block(std::span<std::byte> block, const SampleParameters &parameters,
                                              SampleParameterLayout layout = SampleParameterLayout::a4000_a5000);
void merge_sample_parameters(SampleParameters &destination, const SampleParameters &source);

} // namespace axk::detail
