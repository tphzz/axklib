#include "axklib/application/allocation_map.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <tuple>

namespace {

struct ClusterState {
    bool fixed_used{};
    bool header_used{};
    bool implicit_reserved{};
    bool reconstructed_used{};
    std::vector<axk::app::ImageAllocationOwner> owners;
    std::vector<std::string> flags;
};

void mark_ranges(std::vector<ClusterState> &clusters, const std::vector<axk::AllocationMismatchRange> &ranges,
                 bool ClusterState::*member) {
    for (const auto &range : ranges) {
        if (range.start_cluster >= clusters.size())
            continue;
        const auto end = std::min<std::uint64_t>(range.end_cluster, clusters.size() - 1U);
        for (auto cluster = static_cast<std::uint64_t>(range.start_cluster); cluster <= end; ++cluster)
            clusters[cluster].*member = true;
    }
}

std::string payload_kind_name(axk::PayloadKind kind) {
    switch (kind) {
    case axk::PayloadKind::directory:
        return "DIRECTORY";
    case axk::PayloadKind::object:
        return "OBJECT";
    case axk::PayloadKind::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

axk::app::ImageAllocationOwner
owner_for(const axk::IndexRecord &record, std::string kind, std::optional<std::uint32_t> extent_index,
          const std::unordered_map<std::uint32_t, axk::app::AllocationObjectIdentity> &objects_by_sfs_id,
          const std::unordered_map<std::uint32_t, std::string> &record_names_by_sfs_id) {
    axk::app::ImageAllocationOwner result{std::move(kind),
                                          record.sfs_id.value,
                                          extent_index,
                                          payload_kind_name(record.payload_kind),
                                          std::nullopt,
                                          {},
                                          {},
                                          {},
                                          {}};
    if (const auto object_it = objects_by_sfs_id.find(record.sfs_id.value); object_it != objects_by_sfs_id.end()) {
        result.object_id = object_it->second.object_id;
        result.object_type = object_it->second.object_type;
        result.object_name = object_it->second.object_name;
        result.volume_name = object_it->second.volume_name;
        result.category_name = object_it->second.category_name;
    } else {
        result.object_type = record.object_type;
        result.object_name = record.object_name;
        if (result.object_name.empty()) {
            if (const auto record_name_it = record_names_by_sfs_id.find(record.sfs_id.value);
                record_name_it != record_names_by_sfs_id.end()) {
                result.object_name = record_name_it->second;
            }
        }
        if (result.record_kind == "UNKNOWN" &&
            (result.object_name == "sfserram" || result.object_name == "sfserrlog")) {
            result.record_kind = "SUPPORT";
        }
    }
    return result;
}

std::string allocation_kind(const ClusterState &state) {
    if (state.owners.size() > 1U)
        return "CONFLICT";
    if (state.owners.empty())
        return state.header_used ? "UNCLAIMED" : "FREE";
    const auto &owner = state.owners.front();
    if (owner.claim_kind != "DATA")
        return owner.claim_kind;
    if (owner.record_kind == "DIRECTORY" || owner.record_kind == "SUPPORT" || owner.record_kind == "UNKNOWN")
        return owner.record_kind;
    return "DATA";
}

bool same_run_state(const ClusterState &left, const ClusterState &right) {
    return left.fixed_used == right.fixed_used && left.header_used == right.header_used &&
           left.implicit_reserved == right.implicit_reserved && left.reconstructed_used == right.reconstructed_used &&
           left.owners == right.owners && left.flags == right.flags;
}

} // namespace

axk::Result<axk::app::ImageAllocationMap> axk::app::build_image_allocation_map(
    const Partition &partition, const std::unordered_map<std::uint32_t, AllocationObjectIdentity> &objects_by_sfs_id,
    std::uint32_t sector_size_bytes) {
    if (sector_size_bytes == 0U || partition.sectors_per_cluster == 0U || partition.cluster_count == 0U) {
        return std::unexpected(make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "partition allocation map has invalid geometry"));
    }
    const auto cluster_bytes = static_cast<std::uint64_t>(sector_size_bytes) * partition.sectors_per_cluster;
    if (cluster_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "partition cluster size exceeds supported bounds"));
    }

    std::vector<ClusterState> clusters(partition.cluster_count);
    mark_ranges(clusters, partition.allocation.fixed_location.used_cluster_ranges, &ClusterState::fixed_used);
    mark_ranges(clusters, partition.allocation.header_addressed.used_cluster_ranges, &ClusterState::header_used);

    const auto first_payload = std::min<std::uint64_t>(static_cast<std::uint64_t>(partition.directory_index_cluster) +
                                                           partition.directory_index_span_clusters,
                                                       partition.cluster_count);
    for (std::uint64_t cluster = 0U; cluster < first_payload; ++cluster) {
        clusters[cluster].implicit_reserved = true;
        clusters[cluster].owners.push_back(
            ImageAllocationOwner{"RESERVED", std::nullopt, std::nullopt, "METADATA", std::nullopt, {}, {}, {}, {}});
    }

    std::unordered_map<std::uint32_t, std::string> record_names_by_sfs_id;
    for (const auto &record : partition.records) {
        if (record.payload_kind != PayloadKind::directory)
            continue;
        if (record.directory_id && record.parent_directory_id && record.directory_id == record.parent_directory_id)
            record_names_by_sfs_id.try_emplace(record.sfs_id.value, "/");
        for (const auto &entry : record.directory_entries) {
            if (entry.name != "." && entry.name != ".." && entry.target_link_id)
                record_names_by_sfs_id.try_emplace(entry.target_link_id->value, entry.name);
        }
    }

    ImageAllocationSummary summary;
    summary.total_clusters = partition.cluster_count;
    summary.reserved_clusters = static_cast<std::uint32_t>(first_payload);
    summary.record_count = static_cast<std::uint32_t>(partition.records.size());
    summary.invalid_extent_records = partition.allocation.invalid_extent_record_count;
    for (const auto &record : partition.records) {
        summary.logical_record_bytes += record.data_size;
        summary.total_extent_count += static_cast<std::uint32_t>(record.extents.size());
        summary.maximum_extent_count =
            std::max(summary.maximum_extent_count, static_cast<std::uint32_t>(record.extents.size()));
        if (record.extents.size() > 1U)
            ++summary.fragmented_record_count;
        for (const auto cluster : record.continuation_clusters) {
            if (cluster >= clusters.size())
                continue;
            clusters[cluster].reconstructed_used = true;
            clusters[cluster].owners.push_back(
                owner_for(record, "CONTINUATION", std::nullopt, objects_by_sfs_id, record_names_by_sfs_id));
        }
        for (std::uint32_t extent_index = 0U; extent_index < record.extents.size(); ++extent_index) {
            const auto &extent = record.extents[extent_index];
            if (extent.cluster_offset >= clusters.size())
                continue;
            const auto end = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(extent.cluster_offset) + extent.cluster_count, clusters.size());
            for (auto cluster = static_cast<std::uint64_t>(extent.cluster_offset); cluster < end; ++cluster) {
                clusters[cluster].reconstructed_used = true;
                clusters[cluster].owners.push_back(
                    owner_for(record, "DATA", extent_index, objects_by_sfs_id, record_names_by_sfs_id));
            }
            const auto physical = static_cast<std::uint64_t>(extent.cluster_count) * cluster_bytes;
            if (physical > extent.byte_count)
                summary.data_slack_bytes += physical - extent.byte_count;
        }
    }

    for (auto &state : clusters) {
        std::ranges::sort(state.owners, {}, [](const ImageAllocationOwner &owner) {
            return std::tuple{owner.claim_kind, owner.sfs_id.value_or(0U), owner.extent_index.value_or(0U)};
        });
        if (state.fixed_used != state.header_used) {
            state.flags.emplace_back("BITMAP_COPY_MISMATCH");
            ++summary.bitmap_copy_mismatch_clusters;
        }
        if (state.reconstructed_used && !state.header_used) {
            state.flags.emplace_back("CLAIMED_BUT_FREE");
            ++summary.claimed_but_free_clusters;
        }
        if (state.header_used && !state.reconstructed_used) {
            state.flags.emplace_back("USED_WITHOUT_CLAIM");
            ++summary.used_without_claim_clusters;
        }
        if (state.owners.size() > 1U) {
            state.flags.emplace_back("MULTIPLE_CLAIMS");
            ++summary.conflicting_clusters;
        }
        if (!state.implicit_reserved) {
            if (state.header_used)
                ++summary.allocated_clusters;
            else
                ++summary.free_clusters;
        }
        if (std::ranges::any_of(state.owners, [](const auto &owner) { return owner.claim_kind == "DATA"; }))
            ++summary.data_clusters;
        if (std::ranges::any_of(state.owners, [](const auto &owner) { return owner.claim_kind == "CONTINUATION"; }))
            ++summary.continuation_clusters;
    }
    summary.allocated_bytes = static_cast<std::uint64_t>(summary.allocated_clusters) * cluster_bytes;

    ImageAllocationMap result{partition.index.value,
                              partition.name,
                              sector_size_bytes,
                              partition.sectors_per_cluster,
                              static_cast<std::uint32_t>(cluster_bytes),
                              partition.cluster_count,
                              partition.start_sector,
                              summary,
                              {}};
    for (std::uint32_t start = 0U; start < clusters.size();) {
        std::uint32_t end = start + 1U;
        while (end < clusters.size() && same_run_state(clusters[start], clusters[end]))
            ++end;
        const auto count = end - start;
        result.runs.push_back(
            ImageAllocationRun{start, count,
                               static_cast<std::uint64_t>(partition.start_sector) +
                                   static_cast<std::uint64_t>(start) * partition.sectors_per_cluster,
                               static_cast<std::uint64_t>(count) * partition.sectors_per_cluster,
                               (static_cast<std::uint64_t>(partition.start_sector) * sector_size_bytes) +
                                   static_cast<std::uint64_t>(start) * cluster_bytes,
                               static_cast<std::uint64_t>(count) * cluster_bytes, clusters[start].fixed_used,
                               clusters[start].header_used, clusters[start].reconstructed_used,
                               allocation_kind(clusters[start]), clusters[start].owners, clusters[start].flags});
        if (!clusters[start].implicit_reserved && !clusters[start].header_used) {
            ++result.summary.free_run_count;
            result.summary.largest_free_run_clusters = std::max(result.summary.largest_free_run_clusters, count);
        }
        start = end;
    }
    return result;
}
