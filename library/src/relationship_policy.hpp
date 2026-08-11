#pragma once

#include <functional>
#include <map>
#include <string>

#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"

namespace axk::detail {

[[nodiscard]] bool relationship_has_exact_named_program_target(
    const Relationship &relationship, const std::map<std::string, const ObjectSnapshot *, std::less<>> &objects_by_key);

} // namespace axk::detail
