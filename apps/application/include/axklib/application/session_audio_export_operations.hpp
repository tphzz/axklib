#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {

class DownloadArchiveStore;
class ImageSessionManager;

// Binds audio export inspection and execution for already-open image sessions.
[[nodiscard]] Result<void> bind_session_audio_export_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                                ImageSessionManager &images,
                                                                DownloadArchiveStore &downloads);

} // namespace axk::app
