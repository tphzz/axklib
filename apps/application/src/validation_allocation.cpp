#include "validation_operations_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"

namespace axk::app::validation_operations_internal {

std::uint64_t mismatch_cluster_count(std::span<const axk::AllocationMismatchRange> ranges) {
    std::uint64_t result{};
    for (const auto &range : ranges)
        result += static_cast<std::uint64_t>(range.end_cluster) - range.start_cluster + 1U;
    return result;
}

std::vector<axk::ReportRow> allocation_summary_rows(const std::filesystem::path &path,
                                                    const axk::Container &container) {
    std::vector<axk::ReportRow> rows;
    for (const auto &partition : container.partitions()) {
        const auto cluster_size =
            static_cast<std::uint64_t>(partition.sectors_per_cluster) * container.superblock().sector_size_bytes;
        std::uint64_t direct_records{};
        std::uint64_t continuation_records{};
        std::uint64_t extent_count{};
        std::uint64_t continuation_clusters{};
        std::uint64_t first_payload = partition.cluster_count;
        std::uint64_t first_object = partition.cluster_count;
        for (const auto &record : partition.records) {
            if (record.continuation_clusters.empty())
                ++direct_records;
            else
                ++continuation_records;
            extent_count += record.extents.size();
            continuation_clusters += record.continuation_clusters.size();
            for (const auto &extent : record.extents)
                first_payload = std::min(first_payload, static_cast<std::uint64_t>(extent.cluster_offset));
            if (record.payload_kind == axk::PayloadKind::object ||
                record.payload_kind == axk::PayloadKind::alternating_byte_object) {
                for (const auto &extent : record.extents)
                    first_object = std::min(first_object, static_cast<std::uint64_t>(extent.cluster_offset));
            }
        }
        std::string warnings;
        const auto &allocation = partition.allocation;
        const auto free = allocation.free_space;
        rows.push_back({
            {"source_image", axk::text::path_to_utf8(path)},
            {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
            {"partition_name", partition.name},
            {"start_sector", static_cast<std::uint64_t>(partition.start_sector)},
            {"sectors_per_cluster", static_cast<std::uint64_t>(partition.sectors_per_cluster)},
            {"cluster_count", static_cast<std::uint64_t>(partition.cluster_count)},
            {"bitmap_offset", (static_cast<std::uint64_t>(partition.start_sector) +
                               static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
                                  container.superblock().sector_size_bytes},
            {"index_offset",
             (static_cast<std::uint64_t>(partition.start_sector) +
              static_cast<std::uint64_t>(partition.directory_index_cluster) * partition.sectors_per_cluster) *
                 container.superblock().sector_size_bytes},
            {"scanned_index_bytes", (first_object - partition.directory_index_cluster) * cluster_size},
            {"valid_index_record_count", static_cast<std::uint64_t>(partition.records.size())},
            {"invalid_extent_record_count", static_cast<std::uint64_t>(allocation.invalid_extent_record_count)},
            {"direct_extent_record_count", direct_records},
            {"continuation_extent_record_count", continuation_records},
            {"data_extent_count", extent_count},
            {"continuation_list_cluster_count", continuation_clusters},
            {"stored_used_cluster_count", static_cast<std::uint64_t>(allocation.stored_used_cluster_count)},
            {"reconstructed_used_cluster_count",
             static_cast<std::uint64_t>(allocation.reconstructed_used_cluster_count)},
            {"first_payload_cluster", first_payload},
            {"reserved_cluster_count", free ? static_cast<std::uint64_t>(free->reserved_cluster_count) : first_payload},
            {"sampler_free_cluster_count",
             free ? static_cast<std::uint64_t>(free->free_cluster_count) : std::uint64_t{0}},
            {"sampler_free_bytes", free ? free->free_bytes : std::uint64_t{0}},
            {"sampler_visible_free_kib", free ? free->sampler_visible_free_kib : std::uint64_t{0}},
            {"stored_used_not_reconstructed_count", mismatch_cluster_count(allocation.stored_not_reconstructed)},
            {"reconstructed_used_not_stored_count", mismatch_cluster_count(allocation.reconstructed_not_stored)},
            {"extent_total_mismatch_count", static_cast<std::uint64_t>(allocation.extent_total_mismatch_count)},
            {"conflicting_cluster_count", static_cast<std::uint64_t>(allocation.conflicting_cluster_count)},
            {"conflicts_truncated", allocation.conflicts_truncated},
            {"warning_count", std::uint64_t{0}},
            {"warnings", warnings},
        });
    }
    return rows;
}

std::vector<axk::ReportRow> allocation_extent_rows(const std::filesystem::path &path, const axk::Container &container) {
    std::vector<axk::ReportRow> rows;
    for (const auto &partition : container.partitions()) {
        for (const auto &record : partition.records) {
            for (std::size_t index = 0; index < record.extents.size(); ++index) {
                const auto &extent = record.extents[index];
                rows.push_back({{"source_image", axk::text::path_to_utf8(path)},
                                {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
                                {"sfs_id", static_cast<std::uint64_t>(record.sfs_id.value)},
                                {"record_offset", record.record_offset.value},
                                {"extent_kind", "data"},
                                {"extent_index", static_cast<std::uint64_t>(index)},
                                {"cluster_offset", static_cast<std::uint64_t>(extent.cluster_offset)},
                                {"cluster_count", static_cast<std::uint64_t>(extent.cluster_count)},
                                {"byte_count", static_cast<std::uint64_t>(extent.byte_count)},
                                {"continuation_cluster", nullptr}});
            }
        }
    }
    return rows;
}

std::vector<axk::ReportRow> allocation_mismatch_rows(const std::filesystem::path &path,
                                                     std::span<const axk::Partition> partitions) {
    std::vector<axk::ReportRow> rows;
    const auto append = [&](const axk::Partition &partition, std::string direction,
                            std::span<const axk::AllocationMismatchRange> ranges) {
        for (const auto &range : ranges) {
            rows.push_back(
                {{"source_image", axk::text::path_to_utf8(path)},
                 {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
                 {"partition_name", partition.name},
                 {"direction", direction},
                 {"start_cluster", static_cast<std::uint64_t>(range.start_cluster)},
                 {"end_cluster", static_cast<std::uint64_t>(range.end_cluster)},
                 {"cluster_count", static_cast<std::uint64_t>(range.end_cluster) - range.start_cluster + 1U}});
        }
    };
    for (const auto &partition : partitions) {
        append(partition, "stored-used-without-index-extent", partition.allocation.stored_not_reconstructed);
        append(partition, "index-extent-references-free-cluster", partition.allocation.reconstructed_not_stored);
        const auto claim_kind = [](axk::AllocationClaimKind kind) {
            switch (kind) {
            case axk::AllocationClaimKind::reserved:
                return "reserved";
            case axk::AllocationClaimKind::data:
                return "data";
            case axk::AllocationClaimKind::continuation:
                return "continuation";
            }
            return "unknown";
        };
        for (const auto &conflict : partition.allocation.conflicts) {
            rows.push_back(
                {{"source_image", axk::text::path_to_utf8(path)},
                 {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
                 {"partition_name", partition.name},
                 {"direction", "multiple-allocation-owners"},
                 {"start_cluster", static_cast<std::uint64_t>(conflict.cluster)},
                 {"end_cluster", static_cast<std::uint64_t>(conflict.cluster)},
                 {"cluster_count", std::uint64_t{1}},
                 {"first_owner_kind", claim_kind(conflict.first.kind)},
                 {"first_owner_sfs_id", conflict.first.record
                                            ? axk::ReportValue{static_cast<std::uint64_t>(conflict.first.record->value)}
                                            : axk::ReportValue{nullptr}},
                 {"second_owner_kind", claim_kind(conflict.second.kind)},
                 {"second_owner_sfs_id",
                  conflict.second.record ? axk::ReportValue{static_cast<std::uint64_t>(conflict.second.record->value)}
                                         : axk::ReportValue{nullptr}}});
        }
    }
    return rows;
}

std::vector<axk::ReportRow> volume_validation_rows(const std::filesystem::path &path, const axk::Container &container,
                                                   const axk::ObjectCatalog &catalog,
                                                   std::vector<axk::ReportRow> &detail_issues,
                                                   std::vector<axk::ReportRow> &validation_issues) {
    using VolumeKey = std::tuple<std::uint8_t, std::uint32_t, std::string, std::string>;
    std::map<VolumeKey, std::vector<const axk::ObjectSnapshot *>> volume_objects;
    for (const auto &partition : container.partitions()) {
        std::map<std::uint32_t, const axk::IndexRecord *> directories;
        for (const auto &record : partition.records) {
            if (record.directory_id)
                directories.emplace(record.directory_id->value, &record);
        }
        const axk::IndexRecord *root{};
        for (const auto &[id, directory] : directories) {
            if (directory->parent_directory_id && directory->parent_directory_id->value == id) {
                root = directory;
                break;
            }
        }
        if (root == nullptr || !root->directory_id)
            continue;
        for (const auto &entry : root->directory_entries) {
            const auto found = directories.find(entry.link_id.value);
            if (entry.name == "." || entry.name == ".." || found == directories.end())
                continue;
            const auto *volume = found->second;
            if (!volume->parent_directory_id || volume->parent_directory_id->value != root->directory_id->value)
                continue;
            volume_objects.try_emplace(
                VolumeKey{partition.index.value, volume->sfs_id.value, partition.name, entry.name});
        }
    }
    for (const auto &item : catalog.objects) {
        if (item.placement) {
            volume_objects[{item.partition.value, item.placement->volume_directory.value,
                            item.placement->partition_name, item.placement->volume_name}]
                .push_back(&item);
        }
    }
    std::vector<axk::ReportRow> rows;
    for (const auto &[key, objects] : volume_objects) {
        static_cast<void>(objects);
        const auto &[partition_index, directory_id, partition_name, volume_name] = key;
        const auto partition = std::ranges::find(container.partitions(), partition_index,
                                                 [](const auto &item) { return item.index.value; });
        const axk::IndexRecord *volume_record{};
        if (partition != container.partitions().end()) {
            const auto found = std::ranges::find_if(
                partition->records, [&](const auto &record) { return record.sfs_id.value == directory_id; });
            if (found != partition->records.end())
                volume_record = &*found;
        }
        std::uint64_t category_count{};
        std::uint64_t object_entry_count{};
        std::uint64_t matched_object_count{};
        std::uint64_t category_directory_count{};
        std::uint64_t checked_entry_count{};
        std::uint64_t valid_entry_count{};
        std::uint64_t current_object_count{};
        std::map<axk::ObjectType, std::uint64_t> artifact_counts;
        if (partition != container.partitions().end() && volume_record != nullptr) {
            const auto category_type = [](std::string_view name) {
                if (name == "SMPL")
                    return axk::ObjectType::smpl;
                if (name == "SBNK")
                    return axk::ObjectType::sbnk;
                if (name == "SBAC")
                    return axk::ObjectType::sbac;
                if (name == "PROG")
                    return axk::ObjectType::prog;
                if (name == "SEQU")
                    return axk::ObjectType::sequ;
                return axk::ObjectType::unknown;
            };
            for (const auto &category_entry : volume_record->directory_entries) {
                if (category_entry.name == "." || category_entry.name == "..")
                    continue;
                ++category_count;
                const auto type = category_type(category_entry.name);
                if (type == axk::ObjectType::unknown)
                    continue;
                const auto category = std::ranges::find(partition->records, category_entry.link_id.value,
                                                        [](const auto &record) { return record.sfs_id.value; });
                if (category == partition->records.end())
                    continue;
                ++category_directory_count;
                for (const auto &entry : category->directory_entries) {
                    if (entry.name == "." || entry.name == "..")
                        continue;
                    ++object_entry_count;
                    ++checked_entry_count;
                    const auto target = std::ranges::find(partition->records, entry.link_id.value,
                                                          [](const auto &record) { return record.sfs_id.value; });
                    if (target == partition->records.end() ||
                        (target->payload_kind != axk::PayloadKind::object &&
                         target->payload_kind != axk::PayloadKind::alternating_byte_object))
                        continue;
                    ++matched_object_count;
                    ++valid_entry_count;
                    if (target->payload_kind == axk::PayloadKind::alternating_byte_object)
                        ++artifact_counts[type];
                    else
                        ++current_object_count;
                }
            }
        }
        const auto allocation_issues =
            partition == container.partitions().end()
                ? std::uint64_t{1}
                : static_cast<std::uint64_t>(partition->allocation.invalid_extent_record_count) +
                      partition->allocation.extent_total_mismatch_count +
                      partition->allocation.conflicting_cluster_count +
                      mismatch_cluster_count(partition->allocation.stored_not_reconstructed) +
                      mismatch_cluster_count(partition->allocation.reconstructed_not_stored);
        std::uint64_t artifact_count{};
        for (const auto &[type, count] : artifact_counts) {
            static_cast<void>(type);
            artifact_count += count;
        }
        const auto artifact_smpl_count = artifact_counts[axk::ObjectType::smpl];
        const auto warning_count = artifact_count == 0U ? 0U : 1U;
        const auto details =
            std::format("visible alternating-byte compatibility artifact object entries: "
                        "total={}, SMPL={}, "
                        "SBNK={}, SBAC={}, PROG={}; filesystem tree/allocation validation "
                        "does not prove sampler "
                        "loadability for this physical alternating-byte artifact family",
                        artifact_count, artifact_smpl_count, artifact_counts[axk::ObjectType::sbnk],
                        artifact_counts[axk::ObjectType::sbac], artifact_counts[axk::ObjectType::prog]);
        if (artifact_count != 0U) {
            detail_issues.push_back({
                {"source_image", axk::text::path_to_utf8(path)},
                {"partition_index", static_cast<std::uint64_t>(partition_index)},
                {"partition_name", partition_name},
                {"volume_name", volume_name},
                {"volume_path", "/" + volume_name},
                {"severity", "warning"},
                {"issue_type", "visible-alternating-byte-compatibility-artifact-objects"},
                {"category_code", ""},
                {"category_name", ""},
                {"category_directory_id", ""},
                {"category_directory_path", ""},
                {"entry_offset", ""},
                {"entry_name", ""},
                {"link_id", ""},
                {"target_kind", "object"},
                {"target_sfs_id", ""},
                {"target_payload_kind", "alternating-byte-compatibility-object"},
                {"match_quality", "Likely"},
                {"unmatched_reason", ""},
                {"details", details},
            });
            validation_issues.push_back({
                {"severity", "warning"},
                {"code", "SFS_VOLUME_VISIBLE_ALTERNATING_BYTE_ARTIFACT"},
                {"message", details},
                {"scope", "volume"},
                {"source_path", axk::text::path_to_utf8(path)},
                {"sampler_path", "/" + volume_name},
                {"object_key", ""},
                {"quality", "Likely"},
                {"basis", "axklib.validation.volume"},
                {"recommended_next_check", ""},
            });
        }
        const auto validation_status = allocation_issues != 0U ? "Fail" : warning_count != 0U ? "Warn" : "Pass";
        const auto classification = allocation_issues != 0U ? "volume-likely-corrupt"
                                    : warning_count != 0U   ? "valid-visible-tree-with-warnings"
                                                            : "valid-visible-tree-hidden-unreferenced-not-an-error";
        rows.push_back({
            {"source_image", axk::text::path_to_utf8(path)},
            {"partition_index", static_cast<std::uint64_t>(partition_index)},
            {"partition_name", partition_name},
            {"volume_name", volume_name},
            {"volume_path", "/" + volume_name},
            {"directory_id", static_cast<std::uint64_t>(directory_id)},
            {"category_count", category_count},
            {"object_entry_count", object_entry_count},
            {"matched_object_count", matched_object_count},
            {"category_directory_count", category_directory_count},
            {"checked_category_entry_count", checked_entry_count},
            {"valid_category_entry_count", valid_entry_count},
            {"malformed_category_entry_count", std::uint64_t{0}},
            {"category_count_mismatch_count", std::uint64_t{0}},
            {"current_object_entry_count", current_object_count},
            {"compatibility_artifact_object_entry_count", artifact_count},
            {"compatibility_artifact_smpl_entry_count", artifact_smpl_count},
            {"fatal_issue_count", std::uint64_t{0}},
            {"warning_issue_count", static_cast<std::uint64_t>(warning_count)},
            {"allocation_status", allocation_issues == 0U ? "Pass" : "Fail"},
            {"allocation_issue_count", static_cast<std::uint64_t>(allocation_issues)},
            {"validation_status", validation_status},
            {"volume_classification", classification},
            {"quality_summary", warning_count != 0U ? details
                                : allocation_issues == 0U
                                    ? "category directory entries and optional allocation check passed"
                                    : "allocation check failed"},
        });
    }
    return rows;
}

} // namespace axk::app::validation_operations_internal
