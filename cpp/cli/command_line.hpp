#pragma once

#ifdef _WIN32

#include <span>
#include <string>
#include <vector>

#include "axklib/error.hpp"

namespace axk::cli::platform {

Result<std::vector<std::string>> normalize_windows_command_line(
    std::span<wchar_t* const> arguments);

}  // namespace axk::cli::platform

#endif
