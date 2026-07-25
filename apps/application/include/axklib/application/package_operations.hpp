#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"

namespace axk::app {

class AlterationJournalStore;
class DownloadArchiveStore;
class ImageSessionManager;

// Binds the portable-package command family to the transport-neutral operation registry.
[[nodiscard]] Result<void> bind_package_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                   UploadStore &uploads);
// Binds package operations that act on an already-open mutable image session.
[[nodiscard]] Result<void> bind_session_package_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                           UploadStore &uploads, ImageSessionManager &images,
                                                           AlterationJournalStore &journals,
                                                           DownloadArchiveStore &downloads);

} // namespace axk::app
