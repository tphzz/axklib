#include "package_import_sfs_capacity.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>

namespace axk::package_import_internal {

SfsIndexCapacityEstimate &SfsRecordCapacityPlanner::partition(PartitionCapacity &capacity) {
    const auto partition_index = capacity.partition->index.value;
    const auto [found, inserted] = partitions_.try_emplace(partition_index);
    if (inserted) {
        auto &report = found->second;
        report.partition_index = partition_index;
        report.index_block_count = capacity.index_block_count;
        report.records_per_index_block = capacity.records_per_index_block;
        report.total_record_slots = capacity.total_record_slots;
        report.reserved_record_slots = capacity.reserved_record_slots;
        report.allocatable_record_slots = capacity.allocatable_record_slots;
        report.used_record_slots = capacity.used_record_slots;
        report.free_record_slots = capacity.free_ids.size();
    }
    return found->second;
}

PackageSfsRecordUsage &SfsRecordCapacityPlanner::package(PartitionCapacity &capacity, std::size_t package_index) {
    partition(capacity);
    auto &usage = packages_[capacity.partition->index.value][package_index];
    usage.package_index = package_index;
    return usage;
}

void SfsRecordCapacityPlanner::observe_destination(PartitionCapacity &capacity, std::size_t package_index) {
    auto &usage = package(capacity, package_index);
    usage.volume_scaffolding_record_slots += 6U;
    usage.standalone_required_record_slots += 6U;
    usage.planned_record_slots += 6U;
}

bool SfsRecordCapacityPlanner::allocate_destination(PartitionCapacity &capacity, PlannedPackageDestination &destination,
                                                    std::size_t package_index) {
    auto &usage = package(capacity, package_index);
    const auto remaining = capacity.free_ids.size() - capacity.next_id;
    if (remaining < 6U) {
        usage.allocated_record_slots += remaining;
        usage.shortfall_record_slots += 6U - remaining;
        capacity.next_id = capacity.free_ids.size();
        return false;
    }
    for (std::size_t index = 0U; index < 6U; ++index)
        destination.infrastructure_sfs_ids.push_back(capacity.free_ids[capacity.next_id++]);
    usage.allocated_record_slots += 6U;
    return true;
}

void SfsRecordCapacityPlanner::observe_object(PartitionCapacity &capacity, const PlannedPackageObject &object) {
    auto &usage = package(capacity, object.package_index);
    ++usage.effective_object_record_slots;
    ++usage.standalone_required_record_slots;
    const auto conflicted = std::ranges::contains(object.actions, PackageImportObjectAction::conflict);
    if (std::ranges::contains(object.actions, PackageImportObjectAction::reuse) && !conflicted)
        ++usage.reused_object_count;
    if (!std::ranges::contains(object.actions, PackageImportObjectAction::insert) || conflicted)
        return;
    ++usage.planned_object_record_slots;
    ++usage.planned_record_slots;
}

bool SfsRecordCapacityPlanner::allocate_object(PartitionCapacity &capacity, PlannedPackageObject &object) {
    auto &usage = package(capacity, object.package_index);
    if (capacity.next_id >= capacity.free_ids.size()) {
        ++usage.shortfall_record_slots;
        return false;
    }
    object.target_sfs_id = capacity.free_ids[capacity.next_id++];
    ++usage.allocated_record_slots;
    return true;
}

void SfsRecordCapacityPlanner::finalize(PackageImportPlan &plan) {
    for (auto &[partition_index, report] : partitions_) {
        auto &package_reports = packages_[partition_index];
        for (auto &[package_index, usage] : package_reports) {
            static_cast<void>(package_index);
            report.required_record_slots += usage.planned_record_slots;
            report.allocated_record_slots += usage.allocated_record_slots;
            report.shortfall_record_slots += usage.shortfall_record_slots;
            report.packages.push_back(std::move(usage));
        }
        report.remaining_record_slots = report.free_record_slots - report.allocated_record_slots;
        if (report.shortfall_record_slots != 0U) {
            add_conflict(plan, "SFS_RECORD_CAPACITY_EXHAUSTED",
                         std::format("Partition {} has {} free SFS record slots but this plan requires {}; short by {}",
                                     report.partition_index, report.free_record_slots, report.required_record_slots,
                                     report.shortfall_record_slots));
            plan.conflicts.back().partition_index = report.partition_index;
        }
        plan.sfs_index_capacity.push_back(std::move(report));
    }
}

std::map<std::uint8_t, std::size_t> reusable_root_directory_entries(const Container &container) {
    std::map<std::uint8_t, std::size_t> reusable;
    for (const auto &partition : container.partitions()) {
        const auto root = std::ranges::find(partition.records, SfsId{1}, &IndexRecord::sfs_id);
        if (root == partition.records.end())
            continue;
        reusable[partition.index.value] = static_cast<std::size_t>(
            std::ranges::count(root->directory_entries, DirectoryEntryState::deleted, &DirectoryEntry::state));
    }
    return reusable;
}

void validate_root_directory_growth(const Container &container, std::span<const PlannedPackageDestination> destinations,
                                    PackageImportPlan &plan) {
    for (const auto &partition : container.partitions()) {
        const auto growth = std::ranges::fold_left(
            destinations | std::views::filter([&](const PlannedPackageDestination &destination) {
                return destination.create && destination.partition_index == partition.index.value;
            }) | std::views::transform(&PlannedPackageDestination::root_directory_growth_bytes),
            std::uint64_t{0}, std::plus<>{});
        if (growth == 0U)
            continue;
        const auto root = std::ranges::find(partition.records, SfsId{1}, &IndexRecord::sfs_id);
        if (root == partition.records.end()) {
            add_conflict(plan, "SFS_ROOT_DIRECTORY_MISSING",
                         "partition root directory is unavailable for destination creation");
            continue;
        }
        const auto capacity =
            std::ranges::fold_left(root->extents | std::views::transform([](const Extent &extent) {
                                       return static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
                                   }),
                                   std::uint64_t{0}, std::plus<>{});
        if (root->data_size + growth > capacity) {
            add_conflict(plan, "SFS_ROOT_DIRECTORY_CAPACITY_EXHAUSTED",
                         "partition root directory cannot contain all planned destination volumes");
        }
    }
}

} // namespace axk::package_import_internal
