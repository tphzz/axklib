#pragma once

#include "axklib/application/operation_registry.hpp"
#include <array>

namespace axk::app {
const std::array<OperationDescriptor, 4U> &floppy_import_descriptors();
}
