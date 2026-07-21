#pragma once

#include <filesystem>

#include "axklib/application/contracts.hpp"

namespace axk::app::detail {

[[nodiscard]] Result<void> prepare_private_directory(const std::filesystem::path &path);
[[nodiscard]] Result<void> create_private_file(const std::filesystem::path &path);

} // namespace axk::app::detail
