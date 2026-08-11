#pragma once

#include <optional>
#include <string>

#include <crow.h>

#include "axklib/server/config.hpp"

namespace axk::server::detail {

[[nodiscard]] std::optional<std::string> authenticated_principal(const Config &config, const crow::request &request);
[[nodiscard]] bool origin_allowed(const Config &config, const crow::request &request);

} // namespace axk::server::detail
