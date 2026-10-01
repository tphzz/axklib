#pragma once

#include <algorithm>
#include <ranges>
#include <set>
#include <span>
#include <string>

#include "axklib/deletion.hpp"

namespace axk::deletion_internal {

inline void add_requested_cleanup(std::set<std::string> &selected, const std::set<std::string> &requested,
                                  std::span<const ObjectDeletionImpact> impacts) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &key : requested) {
            if (selected.contains(key))
                continue;
            const auto impact = std::ranges::find(impacts, key, &ObjectDeletionImpact::object_key);
            if (impact != impacts.end() && impact->role == ObjectDeletionRole::dependency &&
                impact->status == ObjectDeletionStatus::optional &&
                std::ranges::all_of(impact->prerequisite_keys,
                                    [&](const auto &prerequisite) { return selected.contains(prerequisite); })) {
                selected.insert(key);
                changed = true;
            }
        }
    }
}

} // namespace axk::deletion_internal
