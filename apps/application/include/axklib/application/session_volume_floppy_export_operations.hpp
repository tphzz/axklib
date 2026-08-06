#pragma once

#include "axklib/application/contracts.hpp"
#include "axklib/writer.hpp"

namespace axk::app {

class DownloadArchiveStore;
class ImageSessionManager;
class OperationRegistry;
class Sandbox;

[[nodiscard]] Result<void> bind_session_volume_floppy_export_operations(OperationRegistry &registry,
                                                                        const Sandbox &sandbox,
                                                                        ImageSessionManager &images,
                                                                        DownloadArchiveStore &downloads,
                                                                        const axk::MediaBuildLimits &media_limits = {});

} // namespace axk::app
