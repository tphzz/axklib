#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "axklib/error.hpp"

namespace axk::detail {

Result<void> validate_system_remix_selection(std::span<const std::byte> global, std::uint8_t slot);

} // namespace axk::detail
