#pragma once

#include "axklib/server/config.hpp"

namespace axk::server::detail {

[[nodiscard]] app::Result<void> validate_download_cache_paths(const Config &config);

} // namespace axk::server::detail
