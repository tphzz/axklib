#pragma once

#include "axklib/application/operation_registry.hpp"
#include <array>

namespace axk::app {
const std::array<OperationDescriptor, 9U> &filesystem_descriptors();
}
