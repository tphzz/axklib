#pragma once

#include "axklib/application/operation_registry.hpp"

namespace axk::app {

class ImageSessionManager;

[[nodiscard]] Result<void> bind_audition_operations(OperationRegistry &registry, ImageSessionManager &images);

} // namespace axk::app
