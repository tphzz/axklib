#include "relationship_policy.hpp"

#include <algorithm>

namespace axk::detail {

bool relationship_has_exact_named_program_target(
    const Relationship &relationship,
    const std::map<std::string, const ObjectSnapshot *, std::less<>> &objects_by_key) {
    if (relationship.assignment_name.empty())
        return false;
    const auto exact_key = [&](std::string_view key) {
        const auto found = objects_by_key.find(key);
        return found != objects_by_key.end() && found->second->object.header.name == relationship.assignment_name;
    };
    return (relationship.target_key && exact_key(*relationship.target_key)) ||
           std::ranges::any_of(relationship.candidate_keys, exact_key);
}

} // namespace axk::detail
