#include "axklib/semantic.hpp"

#include "semantic_support.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace axk {
namespace {

using semantic_detail::find_object;
using semantic_detail::sampler_path;

std::string object_type_name(ObjectType type) {
    switch (type) {
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    case ObjectType::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string join(const std::vector<std::string> &values) {
    std::string result;
    for (const auto &value : values) {
        if (!result.empty())
            result += " | ";
        result += value;
    }
    return result;
}

} // namespace

WaveformOrphanReport analyze_waveform_orphans(const Container &container, const ObjectCatalog &catalog,
                                              const RelationshipGraph &graph) {
    WaveformOrphanReport result;
    std::unordered_map<std::string, const ObjectSnapshot *> objects_by_key;
    objects_by_key.reserve(catalog.objects.size());
    for (const auto &item : catalog.objects)
        objects_by_key.emplace(item.key, &item);

    std::unordered_map<std::string, std::vector<const Relationship *>> parents_by_target;
    std::unordered_map<std::string, std::vector<std::string>> unresolved_relationships_by_scope;
    parents_by_target.reserve(graph.relationships.size());
    for (const auto &relation : graph.relationships) {
        if (relation.target_key)
            parents_by_target[*relation.target_key].push_back(&relation);
        if (relation.scope_key.empty() ||
            (relation.type != "SBNK_LEFT_MEMBER_TO_SMPL" && relation.type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
            relation.quality == RelationshipQuality::known) {
            continue;
        }
        unresolved_relationships_by_scope[relation.scope_key].push_back(
            std::format("Sample relationship is unresolved: {}", relation.source_key));
    }

    std::unordered_set<std::uint8_t> partitions_with_unknown_records;
    for (const auto &partition : container.partitions()) {
        if (std::ranges::any_of(partition.records, [](const IndexRecord &record) {
                return record.sfs_id.value != 0U && record.payload_kind == PayloadKind::unknown;
            })) {
            partitions_with_unknown_records.insert(partition.index.value);
        }
    }
    std::unordered_map<std::uint8_t, std::vector<std::string>> issues_by_partition;
    for (const auto &issue : catalog.issues)
        issues_by_partition[issue.partition.value].push_back(issue.message);

    for (const auto &item : catalog.objects) {
        const auto *wave_data = std::get_if<CurrentSmpl>(&item.object.payload);
        if (wave_data == nullptr)
            continue;
        WaveformOrphanRow row;
        row.partition = item.partition;
        row.waveform_name = item.object.header.name;
        row.object_key = item.key;
        row.sfs_id = item.sfs_id;
        row.wave_data_reference_value = wave_data->wave_data_reference_value.value;
        if (item.placement) {
            row.partition_name = item.placement->partition_name;
            row.volume_name = item.placement->volume_name;
        }

        if (const auto parents = parents_by_target.find(item.key); parents != parents_by_target.end()) {
            for (const auto *relation : parents->second) {
                if ((relation->type != "SBNK_LEFT_MEMBER_TO_SMPL" && relation->type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
                    relation->quality != RelationshipQuality::known) {
                    continue;
                }
                if (const auto sample = objects_by_key.find(relation->source_key); sample != objects_by_key.end()) {
                    row.referencing_samples.push_back(sample->second->placement
                                                          ? std::format("{}/{}", sample->second->placement->volume_name,
                                                                        sample->second->object.header.name)
                                                          : sample->second->object.header.name);
                }
            }
        }
        if (!row.referencing_samples.empty()) {
            std::ranges::sort(row.referencing_samples);
            row.referencing_samples.erase(std::unique(row.referencing_samples.begin(), row.referencing_samples.end()),
                                          row.referencing_samples.end());
            row.status = WaveformStatus::referenced;
            row.basis = "unique authoritative SBNK member-name match";
            ++result.referenced_count;
        } else {
            std::vector<std::string> blockers;
            if (!item.placement)
                blockers.emplace_back("waveform has no exact SMPL directory placement");
            if (partitions_with_unknown_records.contains(item.partition.value)) {
                blockers.emplace_back("partition contains an unresolved allocated record");
            }
            if (const auto issues = issues_by_partition.find(item.partition.value);
                issues != issues_by_partition.end()) {
                blockers.insert(blockers.end(), issues->second.begin(), issues->second.end());
            }
            if (const auto unresolved = unresolved_relationships_by_scope.find(item.scope_key);
                unresolved != unresolved_relationships_by_scope.end()) {
                blockers.insert(blockers.end(), unresolved->second.begin(), unresolved->second.end());
            }
            if (blockers.empty()) {
                row.status = WaveformStatus::known_unreferenced;
                row.basis = "exact SMPL placement and complete current SBNK "
                            "member resolution";
                ++result.known_unreferenced_count;
            } else {
                row.status = WaveformStatus::ambiguous_or_unresolved;
                row.basis = "orphan status withheld because partition "
                            "ownership is unresolved";
                row.notes = join(blockers);
                ++result.ambiguous_or_unresolved_count;
            }
        }
        result.rows.push_back(std::move(row));
    }
    std::ranges::sort(result.rows, {}, [](const WaveformOrphanRow &row) {
        return std::tuple{row.partition.value, row.volume_name, row.waveform_name, row.sfs_id.value};
    });
    return result;
}

bool ValidationReport::valid() const noexcept {
    return std::ranges::none_of(
        issues, [](const ValidationIssue &issue) { return issue.severity == ValidationSeverity::error; });
}

ValidationReport validate_semantics(const Container &container, const ObjectCatalog &catalog,
                                    const RelationshipGraph &graph) {
    ValidationReport result;
    result.coverage.object_count = catalog.objects.size();
    result.coverage.relationship_count = graph.relationships.size();
    for (const auto &item : catalog.objects) {
        if (item.placement) {
            ++result.coverage.exact_placement_count;
            if (item.placement->category_name != object_type_name(item.object.header.type)) {
                result.issues.push_back({
                    "VOL_OBJECT_CATEGORY_MISMATCH",
                    ValidationSeverity::error,
                    std::format("{} object '{}' is stored in the {} category",
                                object_type_name(item.object.header.type), item.object.header.name,
                                item.placement->category_name),
                    sampler_path(item),
                    item.key,
                });
            }
        } else {
            ++result.coverage.unresolved_placement_count;
        }
    }
    for (const auto &issue : catalog.issues) {
        result.issues.push_back({
            issue.code,
            ValidationSeverity::error,
            issue.message,
            std::format("partition {}", issue.partition.value),
            issue.sfs_id ? std::format("p{}:sfs{}", issue.partition.value, issue.sfs_id->value) : "",
        });
    }
    for (const auto &relation : graph.relationships) {
        switch (relation.quality) {
        case RelationshipQuality::known:
            ++result.coverage.known_relationship_count;
            break;
        case RelationshipQuality::likely:
            ++result.coverage.likely_relationship_count;
            break;
        case RelationshipQuality::tentative:
            ++result.coverage.tentative_relationship_count;
            break;
        case RelationshipQuality::unknown:
            ++result.coverage.unknown_relationship_count;
            break;
        }
        if ((relation.type == "SBNK_LEFT_MEMBER_TO_SMPL" || relation.type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
            relation.quality == RelationshipQuality::unknown) {
            const auto *source = find_object(catalog, relation.source_key);
            result.issues.push_back({
                "REL_SBNK_MEMBER_TARGET_MISSING",
                ValidationSeverity::error,
                source == nullptr ? "Sample does not resolve to exactly one Wave Data object"
                                  : std::format("Sample '{}' does not resolve to exactly one Wave Data object",
                                                source->object.header.name),
                source == nullptr ? "" : sampler_path(*source),
                relation.source_key,
            });
        }
        if (relation.type.starts_with("PROG_ASSIGNMENT_TO_") &&
            relation.assignment_state == AssignmentState::stored_assignment && !relation.target_key) {
            const auto *source = find_object(catalog, relation.source_key);
            result.issues.push_back({
                "REL_PROGRAM_STORED_ROW_TARGET_MISSING",
                ValidationSeverity::warning,
                std::format("stored Program assignment row '{}' has no exact local target and is not effective",
                            relation.assignment_name),
                source == nullptr ? "" : sampler_path(*source),
                relation.source_key,
            });
        }
    }
    for (const auto &comparison : graph.bitmap_comparisons) {
        if (comparison.status == "match")
            continue;
        const auto *source = find_object(catalog, comparison.sbnk_key);
        result.issues.push_back({
            "REL_SBNK_PROGRAM_BITMAP_MISMATCH",
            ValidationSeverity::warning,
            "Sample Program bitmap differs from decoded direct Program "
            "assignments",
            source == nullptr ? "" : sampler_path(*source),
            comparison.sbnk_key,
        });
    }
    for (const auto &partition : container.partitions()) {
        const auto partition_path = std::format("partition {}: {}", partition.index.value, partition.name);
        for (const auto &diagnostic : partition.diagnostics) {
            if (diagnostic.code != ErrorCode::relationship_unresolved ||
                diagnostic.context.object_type != "directory-entry") {
                continue;
            }
            const auto entry_name = diagnostic.context.object_name.value_or("<unnamed>");
            result.issues.push_back({
                "SFS_DIRECTORY_ENTRY_TARGET_MISSING",
                ValidationSeverity::error,
                std::format("Directory entry '{}' references a missing SFS record", entry_name),
                std::format("{} / directory entry {}", partition_path, entry_name),
                {},
            });
        }
        for (const auto &record : partition.records) {
            if (record.extent_byte_count_total == record.data_size)
                continue;
            result.issues.push_back({
                "SFS_EXTENT_BYTE_TOTAL_MISMATCH",
                ValidationSeverity::error,
                std::format("SFS record {} declares {} logical byte(s), but its extents declare {} byte(s)",
                            record.sfs_id.value, record.data_size, record.extent_byte_count_total),
                std::format("{} / SFS record {}", partition_path, record.sfs_id.value),
                {},
            });
        }
        if (!partition.allocation.stored_copies_match) {
            result.issues.push_back({
                "SFS_ALLOCATION_BITMAP_COPIES_DIFFER",
                ValidationSeverity::error,
                std::format("the fixed-location and header-addressed SFS allocation bitmaps differ in {} byte(s)",
                            partition.allocation.stored_copy_mismatch_byte_count),
                partition_path,
                {},
            });
        }
        if (partition.allocation.conflicting_cluster_count != 0U) {
            result.issues.push_back({
                "SFS_ALLOCATION_CROSS_LINK",
                ValidationSeverity::error,
                std::format("{} cluster(s) are claimed by multiple SFS allocation owners",
                            partition.allocation.conflicting_cluster_count),
                partition_path,
                {},
            });
        }
        const auto fixed_without_record = partition.allocation.fixed_location.marked_used_without_index_extent_count;
        const auto fixed_marked_free = partition.allocation.fixed_location.index_extent_marked_free_count;
        const auto header_without_record = partition.allocation.header_addressed.marked_used_without_index_extent_count;
        const auto header_marked_free = partition.allocation.header_addressed.index_extent_marked_free_count;
        if (partition.allocation.invalid_extent_record_count != 0U ||
            partition.allocation.extent_total_mismatch_count != 0U || fixed_without_record != 0U ||
            fixed_marked_free != 0U || header_without_record != 0U || header_marked_free != 0U) {
            auto message = std::format(
                "partition allocation metadata disagrees with index extents: fixed bitmap has {} "
                "used-without-extent and {} extent-marked-free cluster(s); header-addressed bitmap has {} "
                "used-without-extent and {} extent-marked-free cluster(s); {} record(s) contain invalid "
                "extents; {} record(s) have extent totals that disagree with their headers",
                fixed_without_record, fixed_marked_free, header_without_record, header_marked_free,
                partition.allocation.invalid_extent_record_count, partition.allocation.extent_total_mismatch_count);
            result.issues.push_back({
                "SFS_ALLOCATION_MISMATCH",
                ValidationSeverity::error,
                std::move(message),
                partition_path,
                {},
            });
        }
    }
    return result;
}

std::string_view waveform_status_name(WaveformStatus status) noexcept {
    switch (status) {
    case WaveformStatus::referenced:
        return "referenced";
    case WaveformStatus::known_unreferenced:
        return "known_unreferenced";
    case WaveformStatus::ambiguous_or_unresolved:
        return "ambiguous_or_unresolved";
    }
    return "ambiguous_or_unresolved";
}

} // namespace axk
