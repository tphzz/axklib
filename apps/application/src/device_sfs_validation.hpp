#pragma once

#include <algorithm>
#include <vector>

#include "axklib/sfs.hpp"
#include "axklib/sfs_allocation.hpp"

namespace axk::app::device_detail {
inline bool valid_sfs_envelope(const Container &image, std::uint64_t extent) {
    if (image.superblock().sector_size_bytes != 512U || !image.backup_superblock_matches() ||
        static_cast<std::uint64_t>(image.superblock().total_sector_count) * 512U != extent ||
        !image.diagnostics().empty() || image.partitions().empty() ||
        static_cast<std::size_t>(std::ranges::count_if(image.superblock().partition_entries, [](const auto &entry) {
            return entry.active();
        })) != image.partitions().size())
        return false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto &partition : image.partitions()) {
        const auto begin = static_cast<std::uint64_t>(partition.start_sector);
        const auto end = begin + partition.sector_count;
        if (begin < 3U || end > extent / 512U || begin >= end || !partition.backup_header_matches ||
            !partition.diagnostics.empty() || !allocation_is_safe_for_mutation(partition.allocation) ||
            std::ranges::any_of(ranges, [&](const auto &range) { return begin < range.second && range.first < end; }))
            return false;
        ranges.emplace_back(begin, end);
    }
    return true;
}
} // namespace axk::app::device_detail
