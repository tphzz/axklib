#include "package_import_support.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "axklib/relationship.hpp"

namespace axk::package_import_internal {
namespace {

bool link_target(const PlannedPackageObject &object) {
    return object.object_type == "SBNK" || object.object_type == "SBAC";
}

auto physical_key(const PlannedPackageObject &object) {
    const auto identity = object.existing_object_key
                              ? "existing:" + *object.existing_object_key
                              : "planned:" + object.canonical_action_id.value_or(object.action_id);
    return std::tuple{object.partition_index, object.volume_name, object.raw_group, object.raw_volume, identity};
}

bool cleared_row(const PackageImportPlan &plan, const PlannedPackageObject &owner, std::uint32_t ordinal) {
    return std::ranges::any_of(plan.program_assignment_adjustments, [&](const auto &adjustment) {
        return adjustment.origin == PackageProgramAssignmentOrigin::imported_program &&
               adjustment.action_id == owner.action_id && adjustment.assignment_ordinal == ordinal;
    });
}

} // namespace

Result<void> plan_program_links(std::span<const PortablePackage> packages, std::span<const ExistingObject> existing,
                                PackageImportPlan &plan) {
    std::vector<const ObjectSnapshot *> snapshots;
    std::map<std::string, const ObjectSnapshot *, std::less<>> by_key;
    for (const auto &item : existing) {
        snapshots.push_back(item.snapshot);
        by_key.emplace(item.snapshot->key, item.snapshot);
    }
    const auto graph = build_relationship_graph(snapshots);
    std::map<std::string, const BitmapComparison *, std::less<>> comparisons;
    for (const auto &comparison : graph.bitmap_comparisons)
        comparisons.emplace(comparison.object_key, &comparison);

    // An imported unresolved row must never clear a live destination assignment on reuse.
    for (const auto &owner : plan.objects) {
        if (owner.object_type != "PROG" || !owner.existing_object_key)
            continue;
        for (const auto &row : graph.relationships) {
            if (row.source_key == *owner.existing_object_key && row.assignment_index && row.target_key &&
                is_program_assignment_row(row) &&
                cleared_row(plan, owner, static_cast<std::uint32_t>(*row.assignment_index))) {
                add_conflict(plan, "PROGRAM_ASSIGNMENT_ADJUSTMENT_CONFLICT",
                             "Cannot reuse an existing Program while clearing a live destination assignment");
            }
        }
    }

    struct Metadata {
        std::set<std::uint8_t> programs;
        bool sample_bank_member{};
    };
    std::map<decltype(physical_key(PlannedPackageObject{})), Metadata> metadata;
    for (const auto &object : plan.objects) {
        if (!link_target(object))
            continue;
        auto &target = metadata[physical_key(object)];
        if (!object.existing_object_key)
            continue;
        const auto found = comparisons.find(*object.existing_object_key);
        const auto snapshot = by_key.find(*object.existing_object_key);
        if (found == comparisons.end() || snapshot == by_key.end())
            return std::unexpected{planner_error("existing Program-link metadata is unavailable")};
        const auto &comparison = *found->second;
        if (comparison.status != "match") {
            add_conflict(plan, "PROGRAM_LINKS_INCONSISTENT",
                         program_bitmap_mismatch_message(comparison, snapshot->second->object.header.name));
            auto &conflict = plan.conflicts.back();
            conflict.partition_index = object.partition_index;
            conflict.volume_name = object.volume_name;
            conflict.node_id = object.node_id;
            continue;
        }
        target.programs.insert(comparison.direct_assignment_programs.begin(),
                               comparison.direct_assignment_programs.end());
        if (const auto *sample = std::get_if<CurrentSbnk>(&snapshot->second->object.payload))
            target.sample_bank_member = (sample->sample_flags & 1U) != 0U;
    }
    for (const auto &owner : plan.objects) {
        if (std::ranges::contains(owner.actions, PackageImportObjectAction::conflict))
            continue;
        for (const auto &edge : packages[owner.package_index].relationships) {
            if (edge.source_node_id != owner.node_id ||
                (edge.role != "SBAC_SLOT_TO_SBNK" && edge.role != "PROG_ASSIGNMENT_TO_SBNK" &&
                 edge.role != "PROG_ASSIGNMENT_TO_SBAC"))
                continue;
            const auto *target = planned_node(plan, owner, edge.target_node_id);
            if (target == nullptr || !link_target(*target))
                continue;
            auto &entry = metadata[physical_key(*target)];
            if (edge.role == "SBAC_SLOT_TO_SBNK") {
                entry.sample_bank_member = true;
            } else {
                if (cleared_row(plan, owner, edge.ordinal))
                    continue;
                const auto number = planned_program_number(owner);
                if (!number)
                    return std::unexpected{number.error()};
                entry.programs.insert(*number);
            }
        }
    }
    for (auto &object : plan.objects) {
        if (!link_target(object))
            continue;
        const auto &entry = metadata.at(physical_key(object));
        object.target_program_numbers.assign(entry.programs.begin(), entry.programs.end());
        object.target_sample_bank_member = object.object_type == "SBNK" && entry.sample_bank_member;
    }
    return {};
}

} // namespace axk::package_import_internal
