#pragma once

#include <nlohmann/json_fwd.hpp>

#include "axklib/alteration.hpp"

namespace axk::detail {
Result<ConvertSampleFormatOperation> parse_sample_format_conversion_json(const nlohmann::json &value,
                                                                         PartitionSelector selector);
Result<void> validate_sample_format_conversion(const ConvertSampleFormatOperation &operation);
} // namespace axk::detail
