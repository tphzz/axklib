#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/contracts.hpp"
#include "axklib/server/config.hpp"

namespace axk::server {

[[nodiscard]] app::Result<nlohmann::json> parse_json_request(std::string_view body, const Config &config);

} // namespace axk::server
