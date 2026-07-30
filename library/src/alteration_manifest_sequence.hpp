#pragma once

#include <filesystem>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "axklib/alteration.hpp"

namespace axk::detail {

Result<AlterationOperationData> parse_sequence_operation_json(const nlohmann::json &row, std::string_view type,
                                                              PartitionSelector selector,
                                                              const std::filesystem::path &base_directory,
                                                              std::string_view context);

} // namespace axk::detail
