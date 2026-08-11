#pragma once

#include "axklib/application/operation_registry.hpp"

namespace axk::app {

class ImageSessionManager;

[[nodiscard]] Result<void> bind_session_placement_operations(OperationRegistry &registry, ImageSessionManager &images,
                                                             OperationRegistry::Handler alter_session);

} // namespace axk::app
