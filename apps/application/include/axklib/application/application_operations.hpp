#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"

namespace axk::app {

// Composes every stateful domain-operation family. Transport adapters should
// depend on this function rather than knowing the individual service modules.
[[nodiscard]] Result<void> bind_application_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                       UploadStore &uploads);
[[nodiscard]] Result<OperationRegistry>
make_application_registry(const Sandbox &sandbox, UploadStore &uploads,
                          OperationRegistry registry = make_operation_registry());

} // namespace axk::app
