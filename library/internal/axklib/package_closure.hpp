#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "axklib/relationship.hpp"

namespace axk::package_internal {

[[nodiscard]] Result<std::vector<const Relationship *>>
required_relationships(const ObjectSnapshot &object, const RelationshipGraph &graph,
                       const std::map<std::string, const ObjectSnapshot *, std::less<>> &objects);

} // namespace axk::package_internal
