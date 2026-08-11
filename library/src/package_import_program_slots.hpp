#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "axklib/package.hpp"

#include "package_import_support.hpp"

namespace axk::package_import_internal {

struct ProgramSlotCandidate {
    std::size_t package_index{};
    std::string node_id;
    DestinationKey destination;
    std::uint8_t source_slot{};
    bool exact_reuse{};
};

struct ProgramSlotPlanningInput {
    std::vector<ProgramSlotCandidate> candidates;
    std::map<DestinationKey, std::set<std::uint8_t>> occupied_slots;
    std::map<std::pair<std::size_t, std::string>, std::uint8_t> assignments;
};

struct SfsImportPolicyMaps {
    std::map<std::pair<std::size_t, std::string>, std::string> renames;
    std::map<std::pair<std::size_t, std::string>, std::uint8_t> program_slots;
};

std::vector<PackageProgramSlotPlacement> plan_program_slot_placements(const ProgramSlotPlanningInput &input);
std::string program_slot_placement_identity(const DestinationKey &destination);
SfsImportPolicyMaps validate_sfs_import_policy(std::span<const PortablePackage> packages,
                                               const PackageImportPolicy &policy, PackageImportPlan &plan);
Result<void>
append_sfs_program_slot_placements(std::span<const Candidate> candidates, std::span<ExistingObject> existing,
                                   const Container &container, RetainedPackageImportStats *stats,
                                   const std::map<std::pair<std::size_t, std::string>, std::uint8_t> &assignments,
                                   PackageImportPlan &plan, const CancellationToken &cancellation);

} // namespace axk::package_import_internal
