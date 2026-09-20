#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "axklib/error.hpp"
#include "axklib/writer.hpp"

namespace axk::detail {

nlohmann::json sample_parameters_json(const SampleParameters &value, std::vector<std::string> *unavailable = nullptr);

Result<SamplePlaybackWindow> parse_sample_playback_window_json(const nlohmann::json &value);

Result<SampleStorageFormat> parse_sample_storage_format(const nlohmann::json &object, std::string_view context);

Result<SampleParameters> parse_sample_parameters_json(const nlohmann::json &value, std::string_view context,
                                                      bool require_nonempty, ErrorCode error_code,
                                                      ErrorCategory error_category);

} // namespace axk::detail
