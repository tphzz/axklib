#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/alteration_transaction.hpp"
#include "axklib/catalog.hpp"
#include "axklib/package.hpp"
#include "axklib/package_relocation.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"

namespace axk::alteration_internal {

struct MutablePartition {
    struct InsertedRecord {
        SfsId id;
        std::vector<std::byte> raw_index;
        std::vector<std::byte> payload;
        std::vector<Extent> extents;
        std::vector<std::uint32_t> continuation_clusters;
        PayloadKind payload_kind{PayloadKind::unknown};
        bool capacity_expanded{};
    };

    const Partition *source{};
    std::optional<std::string> renamed_name;
    std::vector<std::byte> bitmap;
    std::set<SfsId> deleted;
    std::optional<std::vector<std::byte>> root_payload;
    std::optional<std::vector<std::byte>> root_index;
    std::map<SfsId, InsertedRecord> inserted;
    std::map<SfsId, InsertedRecord> changed;
};

struct TransactionState {
    std::shared_ptr<const RandomAccessReader> source;
    Container container;
    ObjectCatalog catalog;
    RelationshipGraph graph;
    std::vector<std::tuple<PartitionIndex, SfsId, SfsId>> known_edges;
    std::map<std::uint8_t, MutablePartition> partitions;
    std::vector<OperationReport> reports;
};

struct OperationContext {
    std::string_view id;
    std::string_view type;
};

struct ExpectedObjectPlacement {
    PartitionIndex partition;
    SfsId sfs_id;
    std::string volume_name;
};

struct ParsedDirectoryEntry {
    SfsId id;
    std::string name;
    std::size_t offset{};
};

struct CategoryObject {
    std::string name;
    SfsId id;
    std::vector<std::byte> payload;
    DecodedObject decoded;
};

Error transaction_error(std::string message);
Error stale_transaction_error(std::string message);
Result<void> require_distinct_source_and_output(const std::filesystem::path &source,
                                                const std::filesystem::path &output, std::string_view operation);
bool requires_object_graph(const AlterationManifest &manifest);
const IndexRecord *record(const Partition &partition, SfsId id);
const MutablePartition::InsertedRecord *current_record(const MutablePartition &partition, SfsId id);
bool record_exists(const MutablePartition &partition, SfsId id);
Result<std::vector<std::byte>> current_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                               const CancellationToken &cancellation);
PayloadKind current_payload_kind(const MutablePartition &partition, SfsId id);
Result<std::vector<ParsedDirectoryEntry>> parse_directory(std::span<const std::byte> payload, SfsId id);
Result<std::vector<std::byte>> read_raw(const RandomAccessReader &reader, std::uint64_t offset, std::size_t size);
Result<std::vector<std::byte>> read_raw(const std::filesystem::path &path, std::uint64_t offset, std::size_t size);
void set_bitmap(std::vector<std::byte> &bitmap, std::uint32_t cluster, bool used);
bool bitmap_used(const std::vector<std::byte> &bitmap, std::uint32_t cluster);
Result<std::vector<std::byte>> current_root_payload(TransactionState &state, MutablePartition &partition,
                                                    const CancellationToken &cancellation);
Result<void> set_root_payload(TransactionState &state, MutablePartition &partition, std::vector<std::byte> payload,
                              const CancellationToken &cancellation);
Result<void> replace_record_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                    std::vector<std::byte> payload, const CancellationToken &cancellation);
Result<SfsId> unique_directory_child(TransactionState &state, MutablePartition &partition, SfsId directory,
                                     std::string_view name, const CancellationToken &cancellation);
Result<SfsId> volume_category(TransactionState &state, MutablePartition &partition, std::string_view volume_name,
                              std::string_view category_name, const CancellationToken &cancellation);
Result<std::pair<SfsId, SfsId>> category_object(TransactionState &state, MutablePartition &partition,
                                                std::string_view volume_name, std::string_view category_name,
                                                std::string_view object_name, std::string_view expected_type,
                                                const CancellationToken &cancellation);
Result<std::vector<CategoryObject>> category_objects(TransactionState &state, MutablePartition &partition,
                                                     std::string_view volume_name, std::string_view category_name,
                                                     ObjectType expected_type, const CancellationToken &cancellation);
Result<void> replace_fixed_object_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                          std::vector<std::byte> payload, const CancellationToken &cancellation);
Result<bool> sbnk_program_bit(std::span<const std::byte> payload, std::uint8_t program);
Result<void> set_sbnk_program_bit(TransactionState &state, MutablePartition &partition, SfsId id, std::uint8_t program,
                                  bool enabled, const CancellationToken &cancellation);
Result<void> set_sbnk_sample_bank_flag(TransactionState &state, MutablePartition &partition, SfsId id, bool enabled,
                                       const CancellationToken &cancellation);
Result<void> remove_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view name, const CancellationToken &cancellation);
Result<PartitionIndex> resolve_partition(const TransactionState &state, const PartitionSelector &selector);
Result<std::set<SfsId>> volume_closure(const Partition &partition, const DirectoryEntry &volume);

std::vector<SfsId> free_ids(const MutablePartition &partition, std::size_t count);
Result<std::vector<Extent>> allocate_extents(MutablePartition &partition, std::uint32_t count);
Result<std::vector<std::uint32_t>> allocate_list_clusters(MutablePartition &partition, std::size_t count);
std::vector<Extent> merge_extents(std::span<const Extent> existing, std::span<const Extent> added);
Result<std::pair<std::uint64_t, std::uint64_t>> grow_directory_capacity(TransactionState &state,
                                                                        MutablePartition &partition, SfsId id,
                                                                        std::uint64_t required_size,
                                                                        const CancellationToken &cancellation);
Result<std::pair<SfsId, std::uint64_t>> allocate_record(MutablePartition &partition, std::vector<std::byte> payload,
                                                        PayloadKind kind, std::optional<SfsId> requested_id = {},
                                                        std::uint16_t directory_tail = 0U);
Result<void> append_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view name, const CancellationToken &cancellation);
Result<void> rename_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view old_name, std::string_view new_name,
                                    const CancellationToken &cancellation);
Result<void> rename_object_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                   std::string_view old_name, std::string_view new_name,
                                   const CancellationToken &cancellation);
Result<std::uint64_t> release_record(MutablePartition &partition, SfsId id);
Result<std::vector<std::byte>> remap_directory(std::vector<std::byte> payload, const std::map<SfsId, SfsId> &mapping);

Result<OperationReport> delete_volume(TransactionState &state, OperationContext context,
                                      const DeleteVolumeOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> insert_volume(TransactionState &state, OperationContext context,
                                      const InsertVolumeOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> rename_volume(TransactionState &state, OperationContext context,
                                      const RenameVolumeOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> rename_partition(TransactionState &state, OperationContext context,
                                         const RenamePartitionOperation &operation,
                                         const CancellationToken &cancellation);
Result<OperationReport> delete_sbnk(TransactionState &state, OperationContext context,
                                    const DeleteSampleOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> insert_sbnk(TransactionState &state, OperationContext context,
                                    const InsertSampleOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> insert_waveform(TransactionState &state, OperationContext context,
                                        const InsertWaveformOperation &operation,
                                        const CancellationToken &cancellation);
Result<OperationReport> delete_waveform(TransactionState &state, OperationContext context,
                                        const DeleteWaveformOperation &operation,
                                        const CancellationToken &cancellation);
void put_padded_name(std::span<std::byte> payload, std::size_t offset, std::string_view name);
Result<OperationReport> rename_waveform(TransactionState &state, OperationContext context,
                                        const RenameWaveformOperation &operation,
                                        const CancellationToken &cancellation);
Result<OperationReport> rename_sbnk(TransactionState &state, OperationContext context,
                                    const RenameSampleOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> delete_program(TransactionState &state, OperationContext context,
                                       const DeleteProgramOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> insert_program(TransactionState &state, OperationContext context,
                                       const InsertProgramOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> rename_program(TransactionState &state, OperationContext context,
                                       const RenameProgramOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> delete_sequence(TransactionState &state, OperationContext context,
                                        const DeleteSequenceOperation &operation,
                                        const CancellationToken &cancellation);
Result<OperationReport> insert_sequence(TransactionState &state, OperationContext context,
                                        const InsertSequenceOperation &operation,
                                        const CancellationToken &cancellation);
Result<OperationReport> rename_sequence(TransactionState &state, OperationContext context,
                                        const RenameSequenceOperation &operation,
                                        const CancellationToken &cancellation);
Result<OperationReport> delete_sbac(TransactionState &state, OperationContext context,
                                    const DeleteSampleBankOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> insert_sbac(TransactionState &state, OperationContext context,
                                    const InsertSampleBankOperation &operation, const CancellationToken &cancellation);
Result<OperationReport> rename_sbac(TransactionState &state, OperationContext context,
                                    const RenameSampleBankOperation &operation, const CancellationToken &cancellation);

Result<std::vector<detail::AlterationPatch>> collect_patches(const TransactionState &state,
                                                             const CancellationToken &cancellation);
Result<void> validate_post_write_placements(const ObjectCatalog &before, const ObjectCatalog &after,
                                            std::span<const ExpectedObjectPlacement> expected_objects);
Result<void> validate_post_write_placements(const TransactionState &state, const Container &actual,
                                            const CancellationToken &cancellation);
Result<TransactionState> open_transaction_state(std::shared_ptr<const RandomAccessReader> source,
                                                const std::filesystem::path &source_path,
                                                const CancellationToken &cancellation, ProgressSink *progress,
                                                bool include_object_graph);
Result<TransactionState> open_transaction_state(const std::filesystem::path &source_path,
                                                const CancellationToken &cancellation, ProgressSink *progress,
                                                bool include_object_graph);
Result<PublicationOutcome>
publish(const TransactionState &state, const std::filesystem::path &output_path, const CancellationToken &cancellation,
        bool overwrite = false,
        const std::function<Result<void>(const std::filesystem::path &)> &temporary_validator = {},
        ProgressSink *progress = nullptr);

bool has_action(const PlannedPackageObject &object, PackageImportObjectAction action);
const PackageNode *package_node(const PortablePackage &package, std::string_view node_id);
const PlannedPackageObject *planned_node(const PackageImportPlan &plan, const PlannedPackageObject &owner,
                                         std::string_view node_id);
Result<package_internal::PackageNodeRelocationContext>
relocation_context(const PortablePackage &package, const PackageImportPlan &plan, const PlannedPackageObject &owner);
Result<std::vector<std::byte>>
clear_program_assignment_adjustments(std::span<const std::byte> payload,
                                     std::span<const PackageProgramAssignmentAdjustment *const> adjustments);
Result<void>
validate_cleared_program_assignment_adjustments(const ObjectSnapshot &snapshot,
                                                std::span<const PackageProgramAssignmentAdjustment *const> adjustments);
Result<void> apply_existing_sfs_program_assignment_adjustments(TransactionState &state, const PackageImportPlan &plan,
                                                               const CancellationToken &cancellation);
Result<std::string> normalized_payload_digest(std::span<const std::byte> payload);
Result<PackageImportReport> apply_fat12_package_import(const std::filesystem::path &target_path,
                                                       std::span<const PortablePackage> packages,
                                                       const PackageImportPlan &plan,
                                                       const std::filesystem::path &output_path, bool overwrite,
                                                       const CancellationToken &cancellation, ProgressSink *progress);
Result<PackageImportReport> apply_iso9660_package_import(const std::filesystem::path &target_path,
                                                         std::span<const PortablePackage> packages,
                                                         const PackageImportPlan &plan,
                                                         const std::filesystem::path &output_path, bool overwrite,
                                                         const CancellationToken &cancellation, ProgressSink *progress);
Result<void> grow_package_category_directories(TransactionState &state, const PackageImportPlan &plan,
                                               const CancellationToken &cancellation);
Result<TransactionState> prepare_sfs_package_import_state(std::shared_ptr<const RandomAccessReader> source,
                                                          const std::filesystem::path &source_path,
                                                          std::span<const PortablePackage> packages,
                                                          const PackageImportPlan &plan,
                                                          std::optional<std::string_view> verified_source_snapshot_id,
                                                          const CancellationToken &cancellation,
                                                          ProgressSink *progress);
std::shared_ptr<const RandomAccessReader> patched_reader(std::shared_ptr<const RandomAccessReader> source,
                                                         std::span<const detail::AlterationPatch> patches);
Result<void> validate_package_result(std::shared_ptr<const RandomAccessReader> reader,
                                     const std::filesystem::path &source_path,
                                     std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                                     const CancellationToken &cancellation);
Result<void> validate_package_result(const std::filesystem::path &temporary, std::span<const PortablePackage> packages,
                                     const PackageImportPlan &plan, const CancellationToken &cancellation);
Result<std::string> file_snapshot_id(const std::filesystem::path &path, const CancellationToken &cancellation);
Result<TransactionState> prepare_alteration(std::shared_ptr<const RandomAccessReader> source,
                                            const std::filesystem::path &source_path,
                                            const AlterationManifest &manifest, const CancellationToken &cancellation,
                                            ProgressSink *progress, std::string_view initial_message,
                                            std::optional<std::string> progress_path = std::nullopt);
Result<TransactionState> prepare_alteration(const std::filesystem::path &source_path,
                                            const AlterationManifest &manifest, const CancellationToken &cancellation,
                                            ProgressSink *progress, std::string_view initial_message,
                                            std::optional<std::string> progress_path = std::nullopt);

} // namespace axk::alteration_internal
