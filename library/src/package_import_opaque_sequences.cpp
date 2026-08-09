#include "package_import_opaque_sequences.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

#include "axklib/package_archive.hpp"

#include "package_import_support.hpp"

namespace axk::package_import_internal {
namespace {

using DecisionKey = std::pair<std::size_t, std::string>;

const PackageOpaqueSequenceDecision *decision_for(const PackageImportPolicy &policy, std::size_t package_index,
                                                  std::string_view node_id) {
    const auto found = std::ranges::find_if(policy.opaque_sequence_decisions, [&](const auto &decision) {
        return decision.package_index == package_index && decision.node_id == node_id;
    });
    return found == policy.opaque_sequence_decisions.end() ? nullptr : &*found;
}

const ObjectSnapshot *catalog_object(std::span<const ObjectSnapshot *const> objects, const CatalogIssue &issue) {
    if (!issue.sfs_id)
        return nullptr;
    const auto found = std::ranges::find_if(objects, [&](const auto *object) {
        return object != nullptr && object->partition == issue.partition && object->sfs_id == *issue.sfs_id;
    });
    return found == objects.end() ? nullptr : *found;
}

bool exact_opaque_target_sequence(const ObjectSnapshot *object) {
    return object != nullptr && object->object.header.raw_type == "SEQU" &&
           object->object.format == ObjectFormat::unknown && object->placement &&
           object->placement_resolution == PlacementResolution::exact && !object->raw_payload.empty();
}

} // namespace

bool is_importable_opaque_sequence(const PortablePackage &package, const PackageNode &node) {
    return node.object_type == "SEQU" && node.object_format == "unknown" && node.relocations.empty() &&
           !node.raw_payload.empty() &&
           std::ranges::find_if(package.relationships, [&](const auto &relationship) {
               return relationship.source_node_id == node.node_id || relationship.target_node_id == node.node_id;
           }) == package.relationships.end();
}

bool skip_opaque_sequence(const PortablePackage &package, const PackageImportPolicy &policy, std::size_t package_index,
                          const PackageNode &node) {
    const auto *decision = decision_for(policy, package_index, node.node_id);
    return decision != nullptr && decision->action == PackageOpaqueSequenceAction::skip &&
           is_importable_opaque_sequence(package, node);
}

void validate_opaque_sequence_policy(std::span<const PortablePackage> packages, const PackageImportPolicy &policy,
                                     PackageImportPlan &plan) {
    std::set<DecisionKey> decision_keys;
    for (const auto &decision : policy.opaque_sequence_decisions) {
        const auto *node = decision.package_index < packages.size()
                               ? node_by_id(packages[decision.package_index], decision.node_id)
                               : nullptr;
        if (!decision_keys.emplace(decision.package_index, decision.node_id).second) {
            add_conflict(plan, "OPAQUE_SEQUENCE_DECISION_DUPLICATE",
                         "an opaque Sequence has more than one import decision");
        } else if (node == nullptr || !is_importable_opaque_sequence(packages[decision.package_index], *node)) {
            add_conflict(plan, "OPAQUE_SEQUENCE_DECISION_INVALID",
                         "opaque Sequence decisions must identify one byte-preserved package Sequence");
        }
    }

    for (std::size_t package_index = 0U; package_index < packages.size(); ++package_index) {
        const auto &package = packages[package_index];
        std::size_t retained_nodes{};
        for (const auto &node : package.nodes) {
            if (!is_importable_opaque_sequence(package, node)) {
                ++retained_nodes;
                continue;
            }
            const auto *decision = decision_for(policy, package_index, node.node_id);
            plan.opaque_sequences.push_back(
                {package_index, node.node_id, node.name, decision ? std::optional{decision->action} : std::nullopt});
            if (decision == nullptr) {
                add_conflict(plan, "OPAQUE_SEQUENCE_DECISION_REQUIRED",
                             "choose whether to preserve the undecodable Sequence unchanged or skip it", nullptr,
                             &package, &node);
                continue;
            }
            const auto skipped = decision->action == PackageOpaqueSequenceAction::skip;
            if (!skipped)
                ++retained_nodes;
            PackageImportWarning warning;
            warning.code = skipped ? "OPAQUE_SEQUENCE_SKIPPED" : "OPAQUE_SEQUENCE_PRESERVED_UNCHANGED";
            warning.message = skipped ? "The undecodable Sequence will not be imported"
                                      : "The undecodable Sequence will be imported without interpreting its events";
            warning.package_index = package_index;
            warning.node_id = node.node_id;
            warning.object_type = node.object_type;
            warning.object_name = node.name;
            plan.warnings.push_back(std::move(warning));
        }
        if (retained_nodes == 0U) {
            add_conflict(plan, "PACKAGE_IMPORT_EMPTY", "skipping opaque Sequences leaves the package empty");
            plan.conflicts.back().package_index = package_index;
            plan.conflicts.back().package_id = package.package_id;
        }
    }
}

void append_sfs_catalog_issues(PackageImportPlan &plan, std::span<const CatalogIssue> issues,
                               std::span<const ObjectSnapshot *const> objects) {
    std::set<std::pair<std::uint8_t, std::uint32_t>> preserved;
    for (const auto &issue : issues) {
        const auto *object = catalog_object(objects, issue);
        if (issue.code != "CATALOG_OBJECT_DECODE_FAILED" || !exact_opaque_target_sequence(object)) {
            add_conflict(plan, issue.code, issue.message);
            plan.conflicts.back().partition_index = issue.partition.value;
            continue;
        }
        const auto key = std::pair{object->partition.value, object->sfs_id.value};
        if (!preserved.emplace(key).second)
            continue;
        PackageImportWarning warning;
        warning.code = "TARGET_SEQUENCE_PRESERVED_OPAQUE";
        warning.message = "An existing undecodable Sequence is unrelated to this import and will remain unchanged";
        warning.origin = PackageImportWarningOrigin::target;
        warning.object_type = "SEQU";
        warning.object_name = object->object.header.name;
        warning.partition_index = object->partition.value;
        warning.volume_name = object->placement->volume_name;
        plan.warnings.push_back(std::move(warning));
        plan.preserved_target_objects.push_back(
            {object->key, object->partition.value, object->sfs_id.value, "SEQU", object->object.header.name,
             object->placement->volume_name, object->placement->category_name, object->placement->entry_name,
             package_internal::hex_digest(package_internal::sha256(object->raw_payload))});
    }
}

void reject_preserved_target_object_use(PackageImportPlan &plan) {
    for (const auto &preserved : plan.preserved_target_objects) {
        const auto used = std::ranges::find_if(plan.objects, [&](const auto &object) {
            return object.partition_index == preserved.partition_index && object.target_sfs_id == preserved.sfs_id;
        });
        if (used == plan.objects.end())
            continue;
        add_conflict(plan, "TARGET_OPAQUE_SEQUENCE_MUTATION_UNSUPPORTED",
                     "the import would use an existing undecodable Sequence that can only be preserved unchanged");
        plan.conflicts.back().partition_index = preserved.partition_index;
        plan.conflicts.back().volume_name = preserved.volume_name;
    }
}

} // namespace axk::package_import_internal
