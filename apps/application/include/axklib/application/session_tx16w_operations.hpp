#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"

namespace axk::app {

[[nodiscard]] Result<void> bind_session_tx16w_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                         UploadStore &uploads, ImageSessionManager &images);

} // namespace axk::app
