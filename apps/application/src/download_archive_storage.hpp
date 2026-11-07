#pragma once

#include <filesystem>

#include "axklib/application/contracts.hpp"

namespace axk::app::detail {

[[nodiscard]] Result<void> prepare_download_archive_storage(const std::filesystem::path &path);

} // namespace axk::app::detail
