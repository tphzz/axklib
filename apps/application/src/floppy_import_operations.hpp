#pragma once

#include "package_operations_internal.hpp"

namespace axk::app {
[[nodiscard]] Result<void>
bind_floppy_import_operations(OperationRegistry &registry, const Sandbox &sandbox, UploadStore &uploads,
                              ImageSessionManager &images,
                              const std::shared_ptr<package_operations_internal::SessionPackageOperationState> &plans);
}
