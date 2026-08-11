#pragma once

#include <nlohmann/json_fwd.hpp>

#include "schema/info_v1.hpp"
#include "schema/operations_v1.hpp"

namespace axk::cli::local_projection {

[[nodiscard]] schema::info_v1::InfoOutput info_output(const nlohmann::json &service_result);
[[nodiscard]] schema::operations_v1::OperationOutput operation_output(const nlohmann::json &operation);

} // namespace axk::cli::local_projection
