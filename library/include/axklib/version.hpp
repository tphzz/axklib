#pragma once

#include <string_view>

#include "axklib/export.hpp"

namespace axk {

[[nodiscard]] AXK_API std::string_view version() noexcept;

} // namespace axk
