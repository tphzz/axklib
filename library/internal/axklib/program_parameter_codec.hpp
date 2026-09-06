#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/object.hpp"
#include "axklib/program_assignment_parameters.hpp"
#include "axklib/program_parameters.hpp"

namespace axk::detail {

enum class ProgramParameterWriteMode { patch, fresh };

bool has_program_parameter_values(const ProgramParameters &parameters);
bool has_program_assignment_parameter_values(const ProgramAssignmentParameters &parameters);

ProgramParameters decode_program_parameters(std::span<const std::byte> payload, const ProgLayout &layout);
Result<void> apply_program_parameters(std::vector<std::byte> &payload, const ProgramParameters &parameters,
                                      ASeriesModel model,
                                      ProgramParameterWriteMode mode = ProgramParameterWriteMode::patch);
ProgramEffectParameters decode_program_effect_parameters(const ProgEffectBlock &effect, std::size_t slot);
Result<void> apply_program_effect_parameters(std::span<std::byte> bytes, const ProgLayout &layout,
                                             const ProgramParameters &parameters, ASeriesModel model,
                                             ProgramParameterWriteMode mode);
std::optional<ProgramReceiveSetting> decode_program_receive(std::uint8_t raw);
Result<std::uint8_t> encode_program_receive(const ProgramReceiveSetting &receive, ASeriesModel model);
ProgramAssignmentParameters decode_program_assignment_parameters(std::span<const std::byte> row,
                                                                 ProgStorageLayout layout);
Result<void> apply_program_assignment_patches(std::vector<std::byte> &payload,
                                              std::span<const ProgramAssignmentParameterPatch> patches,
                                              ASeriesModel model);

} // namespace axk::detail
