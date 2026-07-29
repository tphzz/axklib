#pragma once

#include <string>
#include <string_view>

#include "axklib/application/contracts.hpp"

namespace axk::app::package_internal {

[[nodiscard]] Result<std::string> resolve_filename(std::string_view name, std::string_view required_extension);

} // namespace axk::app::package_internal
