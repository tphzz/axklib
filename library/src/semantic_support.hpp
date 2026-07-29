#pragma once

#include "axklib/semantic.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace axk::semantic_detail {

inline const ObjectSnapshot *find_object(const ObjectCatalog &catalog, std::string_view key) {
    const auto found = std::ranges::find(catalog.objects, key, &ObjectSnapshot::key);
    return found == catalog.objects.end() ? nullptr : &*found;
}

inline std::string sampler_path(const ObjectSnapshot &item) {
    if (!item.placement)
        return std::format("partition {}", item.partition.value);
    return std::format("partition {}: {}/{}", item.partition.value, item.placement->partition_name,
                       item.placement->volume_name);
}

} // namespace axk::semantic_detail
