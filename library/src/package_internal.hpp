#pragma once

#include "axklib/relationship.hpp"

namespace axk::package_internal {

[[nodiscard]] bool
portable_inactive_program_relationship(const Relationship &relationship);

} // namespace axk::package_internal
