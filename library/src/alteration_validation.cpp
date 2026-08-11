#include "alteration_internal.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/catalog_internal.hpp"

namespace axk::alteration_internal {
namespace {

bool is_placement_issue(std::string_view code) {
    return code == "CATALOG_OBJECT_PLACEMENT_MISSING" || code == "CATALOG_OBJECT_PLACEMENT_AMBIGUOUS";
}

auto issue_key(const CatalogIssue &issue) {
    return std::tuple{issue.code, issue.partition.value, issue.sfs_id ? issue.sfs_id->value : 0U};
}

Result<std::vector<ExpectedObjectPlacement>> expected_object_placements(const TransactionState &state) {
    std::vector<ExpectedObjectPlacement> result;
    for (const auto &[partition_index, partition] : state.partitions) {
        for (const auto &[id, record] : partition.inserted) {
            if (record.payload_kind != PayloadKind::object)
                continue;
            const auto report = std::ranges::find_if(state.reports, [&](const OperationReport &candidate) {
                return candidate.partition.value == partition_index &&
                       std::ranges::contains(candidate.inserted_sfs_ids, id);
            });
            if (report == state.reports.end()) {
                return std::unexpected{transaction_error(
                    std::format("post-write inserted object in partition {} SFS ID {} has no operation target",
                                partition_index, id.value))};
            }
            result.push_back({PartitionIndex{partition_index}, id, report->volume_name, false});
        }
    }
    for (const auto &report : state.reports) {
        for (const auto id : report.placed_sfs_ids)
            result.push_back({report.partition, id, report.volume_name, true});
    }
    return result;
}

} // namespace

Result<void> validate_post_write_placements(const ObjectCatalog &before, const ObjectCatalog &after,
                                            std::span<const ExpectedObjectPlacement> expected_objects) {
    std::set<std::tuple<std::string, std::uint8_t, std::uint32_t>> baseline_issues;
    for (const auto &issue : before.issues) {
        if (is_placement_issue(issue.code))
            baseline_issues.insert(issue_key(issue));
    }

    for (const auto &expected : expected_objects) {
        const auto object = std::ranges::find_if(after.objects, [&](const ObjectSnapshot &candidate) {
            return candidate.partition.value == expected.partition.value && candidate.sfs_id == expected.sfs_id;
        });
        if (object == after.objects.end()) {
            return std::unexpected{
                transaction_error(std::format("post-write object in partition {} SFS ID {} could not be reopened",
                                              expected.partition.value, expected.sfs_id.value))};
        }
        if (object->placement_resolution != PlacementResolution::exact || !object->placement) {
            return std::unexpected{transaction_error(
                std::format("post-write object in partition {} SFS ID {} has no exact volume/category placement",
                            expected.partition.value, expected.sfs_id.value))};
        }
        if (object->placement->volume_name != expected.volume_name) {
            return std::unexpected{transaction_error(
                std::format("post-write object in partition {} SFS ID {} is placed in volume '{}' instead of '{}'",
                            expected.partition.value, expected.sfs_id.value, object->placement->volume_name,
                            expected.volume_name))};
        }
        if (object->placement->category_name != object->object.header.raw_type) {
            return std::unexpected{transaction_error(
                std::format("post-write object in partition {} SFS ID {} is placed in category '{}' instead of '{}'",
                            expected.partition.value, expected.sfs_id.value, object->placement->category_name,
                            object->object.header.raw_type))};
        }
        if (expected.preserve_payload) {
            const auto source = std::ranges::find_if(before.objects, [&](const ObjectSnapshot &candidate) {
                return candidate.partition == expected.partition && candidate.sfs_id == expected.sfs_id;
            });
            if (source == before.objects.end() || source->placement_resolution != PlacementResolution::missing ||
                source->raw_payload != object->raw_payload) {
                return std::unexpected{transaction_error(
                    std::format("post-write placement repair changed partition {} SFS ID {} payload or source state",
                                expected.partition.value, expected.sfs_id.value))};
            }
        }
    }

    for (const auto &issue : after.issues) {
        if (!is_placement_issue(issue.code) || baseline_issues.contains(issue_key(issue)))
            continue;
        return std::unexpected{
            transaction_error(std::format("post-write introduced {} for partition {} SFS ID {}", issue.code,
                                          issue.partition.value, issue.sfs_id ? issue.sfs_id->value : 0U))};
    }
    return {};
}

Result<void> validate_post_write_placements(const TransactionState &state, const Container &actual,
                                            const CancellationToken &cancellation) {
    auto after = detail::build_object_catalog(actual, 64U * 1024U * 1024U, cancellation, true);
    if (!after)
        return std::unexpected{after.error()};
    auto expected = expected_object_placements(state);
    if (!expected)
        return std::unexpected{expected.error()};
    return validate_post_write_placements(state.catalog, *after, *expected);
}

} // namespace axk::alteration_internal
