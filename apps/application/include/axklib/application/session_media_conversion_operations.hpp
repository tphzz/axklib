#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/writer.hpp"

namespace axk::app {

class DownloadArchiveStore;
class ImageSessionManager;

// Binds exact SFS partition-to-CD and volume-to-floppy conversion for open images.
[[nodiscard]] Result<void> bind_session_media_conversion_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                                    ImageSessionManager &images,
                                                                    DownloadArchiveStore &downloads,
                                                                    const axk::MediaBuildLimits &media_limits = {});

} // namespace axk::app
