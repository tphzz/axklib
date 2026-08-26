#pragma once

#include "axklib/application/contracts.hpp"

namespace axk::app {

class ImageSessionManager;
class OperationRegistry;

[[nodiscard]] Result<void> bind_image_session_operations(OperationRegistry &registry, ImageSessionManager &images);

} // namespace axk::app
