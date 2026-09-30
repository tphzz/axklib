#include "axklib/capacity_admission_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace axk::detail {
Result<VolumeCapacityAdmission> inspect_capacity_destinations(const Container &overlay,
                                                              std::span<const CapacityDestination> destinations,
                                                              const VolumeCapacityPolicy &policy,
                                                              const CancellationToken &cancellation) {
    VolumeCapacityAdmission result;
    result.target = policy.target;
    std::set<std::pair<std::uint8_t, std::uint32_t>> seen;
    for (const auto &destination : destinations) {
        const auto index = destination.partition;
        const auto &name = destination.name;
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto partition = std::ranges::find(overlay.partitions(), index, &Partition::index);
        if (partition == overlay.partitions().end())
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "Capacity destination partition is missing")};
        const auto root_id = locate_partition_root_record(*partition);
        if (!root_id)
            return std::unexpected{root_id.error()};
        const auto root = std::ranges::find(partition->records, *root_id, &IndexRecord::sfs_id);
        if (root == partition->records.end())
            return std::unexpected{
                make_error(ErrorCode::object_missing, ErrorCategory::object, "Capacity destination root is missing")};
        const DirectoryEntry *placement{};
        for (const auto &entry : root->directory_entries)
            if (entry.state == DirectoryEntryState::live && entry.name == name) {
                if (destination.directory) {
                    const auto record =
                        std::ranges::find(partition->records, entry.target_link_id, &IndexRecord::directory_id);
                    if (record == partition->records.end() || record->sfs_id != *destination.directory)
                        continue;
                }
                if (placement)
                    return std::unexpected{make_error(ErrorCode::relationship_ambiguous, ErrorCategory::relationship,
                                                      "Capacity destination has multiple physical placements")};
                placement = &entry;
            }
        if (!placement || !placement->target_link_id)
            return std::unexpected{
                make_error(ErrorCode::object_missing, ErrorCategory::object, "Capacity destination volume is missing")};
        const auto volume =
            std::ranges::find(partition->records, placement->target_link_id, &IndexRecord::directory_id);
        if (volume == partition->records.end())
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "Capacity destination volume record is missing")};
        if (!seen.emplace(index.value, volume->sfs_id.value).second)
            continue;
        auto report = inspect_volume_capacity(overlay, index, volume->sfs_id, cancellation);
        if (!report)
            return std::unexpected{report.error()};
        const auto profile = std::ranges::find(report->profiles, policy.target, &VolumeCapacityProfile::target);
        if (profile == report->profiles.end())
            return std::unexpected{
                make_error(ErrorCode::invalid_argument, ErrorCategory::manifest, "Invalid A-Series capacity target")};
        if (profile->status != VolumeCapacityStatus::fits)
            result.allowed = false;
        result.reports.push_back(std::move(*report));
    }
    return result;
}
} // namespace axk::detail
