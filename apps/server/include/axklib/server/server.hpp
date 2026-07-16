#pragma once

#include <string_view>

#include "axklib/application/operation_registry.hpp"
#include "axklib/server/config.hpp"

namespace axk::server {

[[nodiscard]] app::Result<int> run(const Config &config,
                                   app::OperationRegistry registry = app::make_operation_registry());
[[nodiscard]] std::string_view embedded_openapi() noexcept;

} // namespace axk::server
