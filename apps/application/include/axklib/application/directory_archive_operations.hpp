#pragma once

#include "axklib/application/operation_registry.hpp"

namespace axk::app {

class DownloadArchiveStore;
class Sandbox;

[[nodiscard]] Result<void> bind_directory_archive_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                             DownloadArchiveStore &downloads);

} // namespace axk::app
