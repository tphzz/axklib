#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {

class DownloadArchiveStore;
class ImageSessionManager;

// Binds the narrow, copy-only repair for malformed SFS extent byte totals.
[[nodiscard]] Result<void> bind_session_extent_layout_repair_operations(OperationRegistry &registry,
                                                                        const Sandbox &sandbox,
                                                                        ImageSessionManager &images,
                                                                        DownloadArchiveStore &downloads);

} // namespace axk::app
