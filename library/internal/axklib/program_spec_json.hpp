#pragma once

#include <nlohmann/json_fwd.hpp>

#include "axklib/error.hpp"
#include "axklib/writer.hpp"

namespace axk::detail {

Result<ProgramSpec> parse_program_spec_json(const nlohmann::json &value);
nlohmann::json program_spec_json(const ProgramSpec &value);

} // namespace axk::detail
