#pragma once

#include <cstddef>
#include <string>

#include "axklib/application/contracts.hpp"

namespace axk::app {

[[nodiscard]] Result<std::string> secure_random_hex(std::size_t byte_count);

} // namespace axk::app
