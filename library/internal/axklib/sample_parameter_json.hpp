#pragma once

#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "axklib/error.hpp"
#include "axklib/writer.hpp"

namespace axk::detail {

Result<SampleParameters> parse_sample_parameters_json(const nlohmann::json &value, std::string_view context,
                                                      bool require_nonempty, ErrorCode error_code,
                                                      ErrorCategory error_category);

} // namespace axk::detail
