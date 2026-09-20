#pragma once

#include "axklib/alteration.hpp"
#include <nlohmann/json_fwd.hpp>

namespace axk::detail {
Result<DuplicateSampleOperation> parse_sample_duplicate_json(const nlohmann::json &value, PartitionSelector selector);
} // namespace axk::detail
