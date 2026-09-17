#pragma once

#include <nlohmann/json_fwd.hpp>

#include "axklib/error.hpp"
#include "axklib/program_assignment_parameters.hpp"
#include "axklib/program_parameters.hpp"

namespace axk::detail {

Result<ProgramParameters> parse_program_parameters_json(const nlohmann::json &value);
Result<ProgramAssignmentParameters> parse_program_assignment_parameters_json(const nlohmann::json &value);
nlohmann::json program_parameters_json(const ProgramParameters &parameters);
nlohmann::json program_assignment_parameters_json(const ProgramAssignmentParameters &parameters);

} // namespace axk::detail
