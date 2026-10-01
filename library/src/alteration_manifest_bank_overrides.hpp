#pragma once
#include "axklib/alteration.hpp"
#include <nlohmann/json.hpp>

namespace axk::detail {
Result<UpdateSampleBankOverridesOperation> parse_bank_overrides_json(const nlohmann::json &row,
                                                                     PartitionSelector selector);
Result<void> validate_bank_overrides_operation(const UpdateSampleBankOverridesOperation &operation);
} // namespace axk::detail
