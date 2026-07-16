#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {

[[nodiscard]] Result<void> bind_file_operations(OperationRegistry &registry, const Sandbox &sandbox);

} // namespace axk::app
