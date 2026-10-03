#pragma once

#include <nlohmann/json_fwd.hpp>

#include "axklib/alteration.hpp"

namespace axk::detail {
Result<ConvertProgramFormatOperation> parse_program_format_conversion_json(const nlohmann::json &value,
                                                                           PartitionSelector selector);
Result<void> validate_program_format_conversion(const ConvertProgramFormatOperation &operation);
} // namespace axk::detail
