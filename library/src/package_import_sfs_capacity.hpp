#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>

#include "package_import_support.hpp"

namespace axk::package_import_internal {

class SfsRecordCapacityPlanner {
  public:
    void observe_destination(PartitionCapacity &capacity, std::size_t package_index);
    [[nodiscard]] bool allocate_destination(PartitionCapacity &capacity, PlannedPackageDestination &destination,
                                            std::size_t package_index);
    void observe_object(PartitionCapacity &capacity, const PlannedPackageObject &object);
    [[nodiscard]] bool allocate_object(PartitionCapacity &capacity, PlannedPackageObject &object);
    void finalize(PackageImportPlan &plan);

  private:
    SfsIndexCapacityEstimate &partition(PartitionCapacity &capacity);
    PackageSfsRecordUsage &package(PartitionCapacity &capacity, std::size_t package_index);

    std::map<std::uint8_t, SfsIndexCapacityEstimate> partitions_;
    std::map<std::uint8_t, std::map<std::size_t, PackageSfsRecordUsage>> packages_;
};

[[nodiscard]] std::map<std::uint8_t, std::size_t> reusable_root_directory_entries(const Container &container);
void validate_root_directory_growth(const Container &container, std::span<const PlannedPackageDestination> destinations,
                                    PackageImportPlan &plan);

} // namespace axk::package_import_internal
