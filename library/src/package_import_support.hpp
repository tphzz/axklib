#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/catalog.hpp"
#include "axklib/package.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/package_import_planning.hpp"
#include "axklib/package_relocation.hpp"

#include "package_import_internal.hpp"

namespace axk::package_import_internal {

using DestinationKey = std::pair<std::uint8_t, std::string>;

struct SfsVolume {
    const Partition *partition{};
    const IndexRecord *directory{};
    std::map<std::string, const IndexRecord *, std::less<>> categories;
};

struct ExistingObject {
    const ObjectSnapshot *snapshot{};
    std::optional<std::string> normalized_sha256;
    std::optional<std::uint32_t> wave_data_reference_value;
    std::optional<std::vector<std::byte>> loaded_payload;
};

struct Candidate {
    const PortablePackage *package{};
    const PackageNode *node{};
    const PackageRootDestination *destination{};
    std::string destination_name;
    std::string projected_normalized_sha256;
};

struct PartitionCapacity {
    const Partition *partition{};
    std::vector<std::uint32_t> free_ids;
    std::set<std::uint32_t> used_clusters;
    std::set<std::uint32_t> used_wave_data_reference_values;
    std::size_t next_id{};
    std::uint32_t next_wave_data_reference_value{0x016b1dbcU};
};

struct ClusterReservation {
    std::uint64_t payload_clusters{};
    std::uint64_t continuation_clusters{};
    std::vector<Extent> extents;
};

Error planner_error(std::string message);
Error stale_plan_error(std::string message);
std::string policy_digest(const PackageImportPolicy &policy);
bool valid_sfs_name(std::string_view value);
std::uint8_t type_rank(std::string_view type);
const PackageNode *node_by_id(const PortablePackage &package, std::string_view node_id);
const PlannedPackageObject *planned_node(const PackageImportPlan &plan, const PlannedPackageObject &owner,
                                         std::string_view node_id);
Result<std::uint8_t> planned_program_number(const PlannedPackageObject &object);
Result<package_internal::PackageNodeRelocationContext>
relocation_context(const PortablePackage &package, const PackageImportPlan &plan, const PlannedPackageObject &owner);
std::vector<const PackageNode *> root_closure(const PortablePackage &package, std::size_t root_index);
std::map<DestinationKey, SfsVolume> sfs_volumes(const Container &container);
std::vector<ExistingObject> existing_objects(const ObjectCatalog &catalog);
std::vector<ExistingObject> retained_existing_objects(std::span<const ObjectSnapshot *const> objects);
std::span<const std::byte> existing_payload(const ExistingObject &object);
Result<void> ensure_existing_identity(ExistingObject &object, const Container &container,
                                      RetainedPackageImportStats *stats, const CancellationToken &cancellation);
void add_conflict(PackageImportPlan &plan, std::string code, std::string message,
                  const PackageRootDestination *destination = nullptr, const PortablePackage *package = nullptr,
                  const PackageNode *node = nullptr);
Result<std::string> projected_normalized_sha256(const PortablePackage &package, const PackageNode &node,
                                                const std::map<std::string, std::string, std::less<>> &names);
PartitionCapacity partition_capacity(const Partition &partition, const ObjectCatalog &catalog);
PartitionCapacity partition_capacity(const Partition &partition,
                                     std::span<const ObjectSnapshot *const> catalog_objects);
std::vector<Extent> cluster_extents(std::span<const std::uint32_t> clusters);
std::vector<Extent> merged_extents(std::span<const Extent> existing, std::span<const Extent> added);
std::optional<ClusterReservation> reserve_clusters(PartitionCapacity &capacity, std::uint32_t payload_cluster_count);
std::optional<ClusterReservation> reserve_directory_growth(PartitionCapacity &capacity,
                                                           std::span<const Extent> existing_extents,
                                                           std::size_t existing_continuation_clusters,
                                                           std::uint32_t additional_payload_clusters);
std::uint64_t remaining_clusters(const PartitionCapacity &capacity);
std::string action_identity(const Candidate &candidate);
void mark_conflict(PlannedPackageObject &object);
bool valid_iso_raw_group(std::string_view value);
bool valid_iso_raw_volume(std::string_view value);
std::optional<std::pair<std::string, std::string>> iso_raw_scope(const ObjectSnapshot &snapshot);

Result<PackageImportPlan> plan_fat12_import(const RandomAccessReader &target_reader,
                                            std::span<const PortablePackage> packages,
                                            const PackageImportRequest &request, const MediaContainer &target,
                                            PackageImportPlan plan, const package_internal::Sha256Digest &before,
                                            bool revalidate_target, const CancellationToken &cancellation);
Result<PackageImportPlan> plan_iso9660_import(const RandomAccessReader &target_reader,
                                              std::span<const PortablePackage> packages,
                                              const PackageImportRequest &request, const MediaContainer &target,
                                              PackageImportPlan plan, const package_internal::Sha256Digest &before,
                                              bool revalidate_target, const CancellationToken &cancellation);
Result<PackageImportPlan> plan_sfs_import(std::shared_ptr<const RandomAccessReader> target_reader,
                                          std::span<const PortablePackage> packages,
                                          const PackageImportRequest &request, const MediaContainer *target,
                                          const RetainedPackageImportTarget *retained_session, PackageImportPlan plan,
                                          const std::optional<package_internal::Sha256Digest> &before,
                                          bool revalidate_target, const CancellationToken &cancellation);

} // namespace axk::package_import_internal
