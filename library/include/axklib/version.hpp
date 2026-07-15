#pragma once

#include <string_view>

#include "axklib/export.hpp"
#include "axklib/sdk/build_info.hpp"

namespace axk {

[[nodiscard]] AXK_API std::string_view version() noexcept;
[[nodiscard]] AXK_API build_info current_build_info() noexcept;

} // namespace axk
