#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace axk::app::job_detail {

[[nodiscard]] std::vector<std::string> destination_keys(const nlohmann::json &request);
[[nodiscard]] bool destinations_overlap(std::string_view left, std::string_view right);
void collect_upload_ids(const nlohmann::json &value, std::vector<std::string> &result);

} // namespace axk::app::job_detail
