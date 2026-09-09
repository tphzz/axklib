#pragma once

#include "axklib/application/operation_registry.hpp"
#include <array>

namespace axk::app {
const std::array<OperationDescriptor, 7U> &filesystem_descriptors();
}
