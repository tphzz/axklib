#pragma once

#include "axklib/application/download_archives.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {
[[nodiscard]] Result<void> bind_filesystem_export_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                             ImageSessionManager &images,
                                                             DownloadArchiveStore &downloads);
} // namespace axk::app
