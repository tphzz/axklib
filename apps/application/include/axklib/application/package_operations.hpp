#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"

namespace axk::app {

// Binds the portable-package command family to the transport-neutral operation registry.
[[nodiscard]] Result<void> bind_package_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                   UploadStore &uploads);

} // namespace axk::app
