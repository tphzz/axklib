#pragma once

#include <string_view>

#include "axklib/application/operation_registry.hpp"
#include "axklib/application/session_format_composition.hpp"
#include "axklib/conversion/profiles.hpp"
#include "axklib/server/config.hpp"

namespace axk::server {

[[nodiscard]] app::Result<int>
run(const Config &config, app::OperationRegistry registry = app::make_operation_registry(),
    app::ConfigureSessionFormats &configure_formats = app::configure_builtin_session_formats,
    conversion::Registry profiles = conversion::builtin_profiles());
[[nodiscard]] std::string_view embedded_openapi() noexcept;

} // namespace axk::server
