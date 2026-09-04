#pragma once

#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "axklib/alteration.hpp"

namespace axk::detail {

Result<void> validate_alteration_manifest(const AlterationManifest &manifest);
Result<AlterationOperationData> parse_sample_bank_assignment_json(const nlohmann::json &row, PartitionSelector selector,
                                                                  std::string_view context);
Result<InsertSampleBankOperation> parse_insert_sample_bank_json(const nlohmann::json &row, PartitionSelector selector,
                                                                std::string_view context);

} // namespace axk::detail
