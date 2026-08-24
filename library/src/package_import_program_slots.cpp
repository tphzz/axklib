#include "package_import_program_slots.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <span>
#include <tuple>

#include "package_internal.hpp"

namespace axk::package_import_internal {
namespace {

std::vector<PackageProgramSlotRange> ranges(std::span<const std::uint8_t> slots) {
    std::vector<PackageProgramSlotRange> result;
    for (const auto slot : slots) {
        if (result.empty() || slot != static_cast<std::uint8_t>(result.back().last + 1U)) {
            result.push_back({slot, slot});
        } else {
            result.back().last = slot;
        }
    }
    return result;
}

std::vector<std::uint8_t> sorted_slots(const std::set<std::uint8_t> &slots) { return {slots.begin(), slots.end()}; }

std::vector<std::uint8_t> suggested_slots(const std::set<std::uint8_t> &occupied, std::size_t required) {
    if (required == 0U)
        return {};
    for (std::uint16_t first = 1U; first + required - 1U <= 128U; ++first) {
        bool available = true;
        for (std::size_t offset = 0U; offset < required; ++offset) {
            if (occupied.contains(static_cast<std::uint8_t>(first + offset))) {
                available = false;
                break;
            }
        }
        if (!available)
            continue;
        std::vector<std::uint8_t> result;
        result.reserve(required);
        for (std::size_t offset = 0U; offset < required; ++offset)
            result.push_back(static_cast<std::uint8_t>(first + offset));
        return result;
    }
    std::vector<std::uint8_t> result;
    result.reserve(required);
    for (std::uint16_t slot = 1U; slot <= 128U && result.size() < required; ++slot) {
        const auto narrowed = static_cast<std::uint8_t>(slot);
        if (!occupied.contains(narrowed))
            result.push_back(narrowed);
    }
    return result;
}

} // namespace

std::string program_slot_placement_identity(const DestinationKey &destination) {
    const auto source = std::format("{}:{}:{}", destination.first, destination.second.size(), destination.second);
    return package_internal::hex_digest(package_internal::sha256(std::as_bytes(std::span{source})));
}

std::vector<PackageProgramSlotPlacement> plan_program_slot_placements(const ProgramSlotPlanningInput &input) {
    std::map<DestinationKey, std::vector<ProgramSlotCandidate>> candidates_by_destination;
    for (const auto &candidate : input.candidates)
        candidates_by_destination[candidate.destination].push_back(candidate);

    std::vector<PackageProgramSlotPlacement> result;
    for (auto &[destination, candidates] : candidates_by_destination) {
        std::ranges::sort(candidates, [](const auto &left, const auto &right) {
            return std::tie(left.package_index, left.source_slot, left.node_id) <
                   std::tie(right.package_index, right.source_slot, right.node_id);
        });
        candidates.erase(std::ranges::unique(candidates, {},
                                             [](const auto &candidate) {
                                                 return std::tuple{candidate.package_index, candidate.node_id};
                                             })
                             .begin(),
                         candidates.end());
        const auto occupied = input.occupied_slots.contains(destination) ? input.occupied_slots.at(destination)
                                                                         : std::set<std::uint8_t>{};
        const auto has_assignments = std::ranges::any_of(candidates, [&](const auto &candidate) {
            return input.assignments.contains({candidate.package_index, candidate.node_id});
        });
        std::vector<ProgramSlotCandidate> participants;
        std::ranges::copy_if(candidates, std::back_inserter(participants), [&](const auto &candidate) {
            return !candidate.exact_reuse || input.assignments.contains({candidate.package_index, candidate.node_id});
        });
        std::set<std::uint8_t> incoming_slots;
        bool collision{};
        for (const auto &candidate : participants) {
            collision = collision || occupied.contains(candidate.source_slot) ||
                        !incoming_slots.emplace(candidate.source_slot).second;
        }
        if (!has_assignments && !collision)
            continue;

        PackageProgramSlotPlacement placement;
        placement.placement_id = program_slot_placement_identity(destination);
        placement.partition_index = destination.first;
        placement.volume_name = destination.second;
        placement.applied = has_assignments;
        placement.required_slot_count = static_cast<std::uint64_t>(participants.size());
        placement.available_slot_count = static_cast<std::uint16_t>(128U - occupied.size());
        placement.occupied_ranges = ranges(sorted_slots(occupied));
        std::vector<std::uint8_t> sources;
        sources.reserve(participants.size());
        for (const auto &candidate : participants)
            sources.push_back(candidate.source_slot);
        std::ranges::sort(sources);
        sources.erase(std::ranges::unique(sources).begin(), sources.end());
        placement.source_ranges = ranges(sources);

        std::vector<std::uint8_t> destinations;
        if (has_assignments) {
            destinations.reserve(participants.size());
            for (const auto &candidate : participants) {
                const auto assigned = input.assignments.find({candidate.package_index, candidate.node_id});
                if (assigned == input.assignments.end())
                    break;
                destinations.push_back(assigned->second);
            }
        } else if (participants.size() <= placement.available_slot_count) {
            destinations = suggested_slots(occupied, participants.size());
        }
        if (destinations.size() != participants.size()) {
            placement.mode = PackageProgramSlotPlacementMode::unavailable;
        } else {
            auto sorted_destinations = destinations;
            std::ranges::sort(sorted_destinations);
            sorted_destinations.erase(std::ranges::unique(sorted_destinations).begin(), sorted_destinations.end());
            placement.destination_ranges = ranges(sorted_destinations);
            placement.mode =
                placement.destination_ranges.size() == 1U &&
                        placement.destination_ranges.front().last - placement.destination_ranges.front().first + 1U ==
                            participants.size()
                    ? PackageProgramSlotPlacementMode::contiguous
                    : PackageProgramSlotPlacementMode::fragmented;
            placement.suggested_start_slot = sorted_destinations.front();
            std::map<std::uint8_t, std::size_t> destination_counts;
            for (const auto destination_slot : destinations)
                ++destination_counts[destination_slot];
            for (std::size_t index = 0U; index < participants.size(); ++index) {
                const auto &candidate = participants[index];
                placement.mappings.push_back(
                    {candidate.package_index, candidate.node_id, candidate.source_slot, destinations[index],
                     occupied.contains(destinations[index]) || destination_counts[destinations[index]] > 1U});
            }
        }
        result.push_back(std::move(placement));
    }
    return result;
}

SfsImportPolicyMaps validate_sfs_import_policy(std::span<const PortablePackage> packages,
                                               const PackageImportPolicy &policy, PackageImportPlan &plan) {
    SfsImportPolicyMaps result;
    for (const auto &rename : policy.renames) {
        const auto *node = rename.package_index < packages.size()
                               ? node_by_id(packages[rename.package_index], rename.node_id)
                               : nullptr;
        if (node == nullptr) {
            add_conflict(plan, "RENAME_NODE_INVALID", "rename references a missing package node");
            continue;
        }
        const auto key = std::pair{rename.package_index, rename.node_id};
        if (node->object_type == "PROG") {
            add_conflict(plan, "SFS_PROGRAM_SLOT_RENAME_UNSUPPORTED",
                         "SFS Program destinations must use explicit Program slot assignments");
        } else if (!valid_sfs_name(rename.destination_name)) {
            add_conflict(plan, "RENAME_NAME_INVALID", "SFS destination names must contain 1 to 16 ASCII bytes");
        } else if (!result.renames.emplace(key, rename.destination_name).second) {
            add_conflict(plan, "RENAME_NODE_DUPLICATE", "package node has more than one rename");
        }
    }
    for (const auto &assignment : policy.program_slot_assignments) {
        const auto *node = assignment.package_index < packages.size()
                               ? node_by_id(packages[assignment.package_index], assignment.node_id)
                               : nullptr;
        const auto key = std::pair{assignment.package_index, assignment.node_id};
        if (node == nullptr || node->object_type != "PROG") {
            add_conflict(plan, "PROGRAM_SLOT_NODE_INVALID",
                         "Program slot assignment references a missing or non-Program package node");
        } else if (assignment.destination_slot < 1U || assignment.destination_slot > 128U) {
            add_conflict(plan, "PROGRAM_SLOT_INVALID", "Program destination slots must be between 001 and 128");
        } else if (!result.program_slots.emplace(key, assignment.destination_slot).second) {
            add_conflict(plan, "PROGRAM_SLOT_NODE_DUPLICATE",
                         "package Program node has more than one destination slot assignment");
        }
    }
    return result;
}

Result<void>
append_sfs_program_slot_placements(std::span<const Candidate> candidates, std::span<ExistingObject> existing,
                                   const Container &container, RetainedPackageImportStats *stats,
                                   const std::map<std::pair<std::size_t, std::string>, std::uint8_t> &assignments,
                                   PackageImportPlan &plan, const CancellationToken &cancellation) {
    ProgramSlotPlanningInput input;
    input.assignments = assignments;
    for (const auto &object : existing) {
        if (!object.snapshot->placement || object.snapshot->object.header.raw_type != "PROG")
            continue;
        PlannedPackageObject program;
        program.destination_name = object.snapshot->object.header.name;
        const auto slot = planned_program_number(program);
        if (slot) {
            input.occupied_slots[{object.snapshot->partition.value, object.snapshot->placement->volume_name}].insert(
                *slot);
        }
    }
    for (const auto &candidate : candidates) {
        if (candidate.node->object_type != "PROG")
            continue;
        PlannedPackageObject source;
        source.destination_name = candidate.node->name;
        const auto source_slot = planned_program_number(source);
        if (!source_slot)
            return std::unexpected{source_slot.error()};
        const auto policy_key = std::pair{candidate.destination->package_index, candidate.node->node_id};
        bool exact_reuse{};
        if (!assignments.contains(policy_key)) {
            ExistingObject *match{};
            std::size_t match_count{};
            for (auto &object : existing) {
                if (!object.snapshot->placement ||
                    object.snapshot->partition.value != *candidate.destination->partition_index ||
                    object.snapshot->placement->volume_name != candidate.destination->volume_name ||
                    object.snapshot->object.header.raw_type != "PROG" ||
                    object.snapshot->object.header.name != candidate.destination_name) {
                    continue;
                }
                match = &object;
                ++match_count;
            }
            if (match_count == 1U) {
                if (const auto loaded = ensure_existing_identity(*match, container, stats, cancellation); !loaded)
                    return std::unexpected{loaded.error()};
                const auto adjusted_reuse = !candidate.cleared_program_assignment_ordinals.empty() &&
                                            match->normalized_sha256 == candidate.unadjusted_normalized_sha256;
                exact_reuse = match->normalized_sha256 == candidate.projected_normalized_sha256 || adjusted_reuse;
            }
        }
        input.candidates.push_back({candidate.destination->package_index,
                                    candidate.node->node_id,
                                    {*candidate.destination->partition_index, candidate.destination->volume_name},
                                    *source_slot,
                                    exact_reuse});
    }
    plan.program_slot_placements = plan_program_slot_placements(input);
    for (const auto &placement : plan.program_slot_placements) {
        if (placement.mode != PackageProgramSlotPlacementMode::unavailable)
            continue;
        const DestinationKey destination{placement.partition_index, placement.volume_name};
        const auto destination_has_assignments = std::ranges::any_of(input.candidates, [&](const auto &candidate) {
            return candidate.destination == destination &&
                   assignments.contains({candidate.package_index, candidate.node_id});
        });
        const auto assignments_incomplete =
            destination_has_assignments && std::ranges::any_of(input.candidates, [&](const auto &candidate) {
                const auto key = std::pair{candidate.package_index, candidate.node_id};
                const auto participates = !candidate.exact_reuse || assignments.contains(key);
                return candidate.destination == destination && participates && !assignments.contains(key);
            });
        add_conflict(plan,
                     assignments_incomplete ? "SFS_PROGRAM_SLOT_ASSIGNMENTS_INCOMPLETE"
                                            : "SFS_PROGRAM_SLOT_CAPACITY_EXHAUSTED",
                     assignments_incomplete
                         ? "Program slot assignments must cover every non-reused Program in the destination volume"
                         : "destination volume does not have enough usable Program slots for the package");
        auto &conflict = plan.conflicts.back();
        conflict.partition_index = placement.partition_index;
        conflict.volume_name = placement.volume_name;
    }
    return {};
}

} // namespace axk::package_import_internal
