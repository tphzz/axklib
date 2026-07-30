#pragma once

#include "axklib/application/contracts.hpp"

namespace axk::app {

class DownloadArchiveStore;
class ImageSessionManager;
class OperationRegistry;
class Sandbox;

[[nodiscard]] Result<void> bind_session_sequence_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                            ImageSessionManager &images,
                                                            DownloadArchiveStore &downloads);

} // namespace axk::app
