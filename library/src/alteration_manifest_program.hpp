#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "axklib/alteration.hpp"

namespace axk::detail {

Result<void> validate_program_assignment_replacement(const ReplaceProgramAssignmentsOperation &operation);
Result<ReplaceProgramAssignmentsOperation> parse_program_assignment_replacement_json(const nlohmann::json &row,
                                                                                     PartitionSelector selector);
Result<std::vector<std::byte>> replace_prog_assignment_rows(std::span<const std::byte> payload,
                                                            const ReplaceProgramAssignmentsOperation &operation);

Result<InsertProgramOperation> parse_insert_program_json(const nlohmann::json &row, PartitionSelector selector,
                                                         std::string_view context);
Result<UpdateProgramParametersOperation> parse_program_parameter_update_json(const nlohmann::json &row,
                                                                             PartitionSelector selector);
Result<ClearProgramAssignmentsOperation>
parse_clear_program_assignments_json(const nlohmann::json &row, PartitionSelector selector, std::string_view context);

} // namespace axk::detail
