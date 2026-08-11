#pragma once

#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "axklib/alteration.hpp"

namespace axk::detail {

Result<InsertProgramOperation> parse_insert_program_json(const nlohmann::json &row, PartitionSelector selector,
                                                         std::string_view context);

} // namespace axk::detail
