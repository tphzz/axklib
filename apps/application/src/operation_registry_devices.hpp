#pragma once

#include <array>

#include "axklib/application/device_sessions.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {
const std::array<OperationDescriptor, 9U> &device_descriptors();
Result<void> bind_device_operations(OperationRegistry &registry, DeviceSessionManager &devices);
} // namespace axk::app
