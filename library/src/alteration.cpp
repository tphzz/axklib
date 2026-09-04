#include "axklib/alteration.hpp"

#include "axklib/utf8.hpp"

#include <algorithm>
#include <concepts>
#include <format>
#include <ranges>
#include <set>
#include <tuple>

#include "alteration_internal.hpp"
#include "alteration_manifest_internal.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/package_relocation.hpp"

namespace axk {

using namespace alteration_internal;

namespace alteration_internal {

std::optional<PartitionIndex> placement_repair_partition(const AlterationManifest &manifest) {
    for (const auto &operation : manifest.operations) {
        const auto *repair = std::get_if<RepairObjectPlacementsOperation>(&operation.data);
        if (repair == nullptr)
            continue;
        return std::get<PartitionIndex>(repair->partition);
    }
    return std::nullopt;
}

Result<TransactionState> prepare_alteration(std::shared_ptr<const RandomAccessReader> source,
                                            const std::filesystem::path &source_path,
                                            const AlterationManifest &manifest, const CancellationToken &cancellation,
                                            ProgressSink *progress, std::string_view initial_message,
                                            std::optional<std::string> progress_path) {
    if (auto valid = detail::validate_alteration_manifest(manifest); !valid)
        return std::unexpected{valid.error()};
    auto opened = open_transaction_state(std::move(source), source_path, cancellation, progress,
                                         requires_object_graph(manifest), placement_repair_partition(manifest));
    if (!opened)
        return std::unexpected{opened.error()};
    auto state = std::move(*opened);
    auto deletion_batch = plan_volume_deletion_batch(state, manifest, cancellation);
    if (!deletion_batch)
        return std::unexpected{deletion_batch.error()};
    state.approved_volume_deletion_batch = std::move(*deletion_batch);
    if (progress) {
        progress->report(
            {ProgressPhase::allocating, 0U, manifest.operations.size(), std::string{initial_message}, progress_path});
    }
    for (std::size_t operation_index = 0; operation_index < manifest.operations.size(); ++operation_index) {
        const auto &typed_operation = manifest.operations[operation_index];
        const auto operation_type = operation_type_name(typed_operation.data);
        const OperationContext context{typed_operation.id, operation_type};
        auto report = std::visit(
            [&](const auto &operation) -> Result<OperationReport> {
                using T = std::decay_t<decltype(operation)>;
                if constexpr (std::same_as<T, DeleteVolumeOperation>)
                    return delete_volume(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, InsertVolumeOperation>)
                    return insert_volume(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, DeleteSampleOperation>)
                    return delete_sbnk(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, InsertSampleOperation>)
                    return insert_sbnk(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, UpdateSampleParametersOperation>)
                    return update_sbnk_parameters(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, InsertWaveformOperation>)
                    return insert_waveform(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, DeleteWaveformOperation>)
                    return delete_waveform(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, RenameWaveformOperation>)
                    return rename_waveform(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, RenameSampleOperation>)
                    return rename_sbnk(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, DeleteSampleBankOperation>)
                    return delete_sbac(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, InsertSampleBankOperation>)
                    return insert_sbac(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, AssignSampleBankMembersOperation>)
                    return assign_sbac_members(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, RenameSampleBankOperation>)
                    return rename_sbac(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, DeleteProgramOperation>)
                    return delete_program(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, InsertProgramOperation>)
                    return insert_program(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, RenameProgramOperation>)
                    return rename_program(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, ClearProgramAssignmentsOperation>)
                    return clear_program_assignments(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, DeleteSequenceOperation>)
                    return delete_sequence(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, InsertSequenceOperation>)
                    return insert_sequence(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, RenameSequenceOperation>)
                    return rename_sequence(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, RenameVolumeOperation>)
                    return rename_volume(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, RepairObjectPlacementsOperation>)
                    return repair_object_placements(state, context, operation, cancellation);
                else if constexpr (std::same_as<T, ImportTx16wDiskSetOperation>)
                    return import_tx16w_disk_set(state, context, operation, cancellation);
                else
                    return rename_partition(state, context, operation, cancellation);
            },
            typed_operation.data);
        if (!report)
            return std::unexpected{report.error()};
        state.reports.push_back(std::move(*report));
        if (progress) {
            progress->report({ProgressPhase::allocating, operation_index + 1U, manifest.operations.size(),
                              std::string{operation_type}, progress_path});
        }
    }
    return state;
}

Result<TransactionState> prepare_alteration(const std::filesystem::path &source_path,
                                            const AlterationManifest &manifest, const CancellationToken &cancellation,
                                            ProgressSink *progress, std::string_view initial_message,
                                            std::optional<std::string> progress_path) {
    auto source = FileReader::open(source_path);
    if (!source)
        return std::unexpected{source.error()};
    return prepare_alteration(*source, source_path, manifest, cancellation, progress, initial_message,
                              std::move(progress_path));
}

} // namespace alteration_internal

Result<detail::PreparedAlteration> detail::prepare_hds_alteration(std::shared_ptr<const RandomAccessReader> source,
                                                                  std::filesystem::path source_path,
                                                                  const AlterationManifest &manifest,
                                                                  const CancellationToken &cancellation,
                                                                  ProgressSink *progress) {
    auto prepared = prepare_alteration(std::move(source), source_path, manifest, cancellation, progress,
                                       "planning alteration", text::path_to_utf8(source_path));
    if (!prepared)
        return std::unexpected{prepared.error()};
    auto patches = collect_patches(*prepared, cancellation);
    if (!patches)
        return std::unexpected{patches.error()};
    OpenOptions options;
    options.cancellation = cancellation;
    auto overlay = patched_reader(prepared->source, *patches);
    auto actual = open_image(std::move(overlay), source_path, options);
    if (!actual)
        return std::unexpected{actual.error()};
    if (auto placements = validate_post_write_placements(*prepared, *actual, cancellation); !placements)
        return std::unexpected{placements.error()};
    return PreparedAlteration{std::move(source_path), prepared->container.image_size_bytes(),
                              std::move(prepared->reports), std::move(*patches)};
}

namespace {

Result<detail::PreparedPackageImport>
prepare_sfs_package_import_impl(std::shared_ptr<const RandomAccessReader> source, std::filesystem::path source_path,
                                std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                                std::optional<std::string_view> verified_source_snapshot_id,
                                const CancellationToken &cancellation, ProgressSink *progress) {
    if (!source)
        return std::unexpected{transaction_error("package import source reader is required")};
    if (const auto verified = verify_package_import_plan(plan); !verified)
        return std::unexpected{verified.error()};
    if (!plan.valid())
        return std::unexpected{transaction_error("a conflicting package import plan cannot apply")};
    if (plan.target_kind != MediaKind::sfs)
        return std::unexpected{transaction_error("journaled package import requires an SFS target")};
    if (packages.size() != plan.package_ids.size())
        return std::unexpected{transaction_error("package import inputs do not match the planned package count")};
    for (std::size_t index = 0; index < packages.size(); ++index) {
        if (const auto verified = verify_portable_package(packages[index]); !verified)
            return std::unexpected{verified.error()};
        if (packages[index].package_id != plan.package_ids[index])
            return std::unexpected{transaction_error("package import input identity differs from the plan")};
    }
    auto prepared = prepare_sfs_package_import_state(source, source_path, packages, plan, verified_source_snapshot_id,
                                                     cancellation, progress);
    if (!prepared)
        return std::unexpected{prepared.error()};
    const auto image_size_bytes = prepared->container.image_size_bytes();
    auto patches = collect_patches(*prepared, cancellation);
    if (!patches)
        return std::unexpected{patches.error()};
    auto overlay = patched_reader(source, *patches);
    if (auto validated = validate_package_result(std::move(overlay), source_path, packages, plan, cancellation);
        !validated) {
        return std::unexpected{validated.error()};
    }
    return detail::PreparedPackageImport{std::move(source_path),  image_size_bytes, plan.plan_id,
                                         plan.target_snapshot_id, plan.objects,     plan.allocation,
                                         std::move(*patches)};
}

} // namespace

Result<detail::PreparedPackageImport>
detail::prepare_sfs_package_import(std::shared_ptr<const RandomAccessReader> source, std::filesystem::path source_path,
                                   std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                                   const CancellationToken &cancellation, ProgressSink *progress) {
    return prepare_sfs_package_import_impl(std::move(source), std::move(source_path), packages, plan, std::nullopt,
                                           cancellation, progress);
}

Result<detail::PreparedPackageImport> detail::prepare_sfs_package_import_verified(
    std::shared_ptr<const RandomAccessReader> source, std::filesystem::path source_path,
    std::span<const PortablePackage> packages, const PackageImportPlan &plan,
    std::string_view verified_source_snapshot_id, const CancellationToken &cancellation, ProgressSink *progress) {
    return prepare_sfs_package_import_impl(std::move(source), std::move(source_path), packages, plan,
                                           verified_source_snapshot_id, cancellation, progress);
}

Result<AlterationResult> alter_hds(const std::filesystem::path &source_path, const AlterationManifest &manifest,
                                   const std::filesystem::path &output_path, const CancellationToken &cancellation,
                                   ProgressSink *progress, bool overwrite) {
    if (auto valid = detail::validate_alteration_manifest(manifest); !valid)
        return std::unexpected{valid.error()};
    if (auto distinct = require_distinct_source_and_output(source_path, output_path, "alteration"); !distinct)
        return std::unexpected{distinct.error()};
    auto prepared = prepare_alteration(source_path, manifest, cancellation, progress, "planning alteration",
                                       text::path_to_utf8(output_path));
    if (!prepared)
        return std::unexpected{prepared.error()};
    auto state = std::move(*prepared);
    auto applied = publish(state, output_path, cancellation, overwrite, {}, progress);
    if (!applied)
        return std::unexpected{applied.error()};
    return AlterationResult{source_path, output_path, true, std::move(state.reports), std::move(*applied)};
}

Result<AlterationInspection> inspect_hds_alteration(const std::filesystem::path &source_path,
                                                    const AlterationManifest &manifest,
                                                    const CancellationToken &cancellation, ProgressSink *progress) {
    auto prepared = prepare_alteration(source_path, manifest, cancellation, progress, "inspecting alteration");
    if (!prepared)
        return std::unexpected{prepared.error()};
    return AlterationInspection{source_path, std::move(prepared->reports)};
}

Result<PackageImportReport> apply_package_import(const std::filesystem::path &target_path,
                                                 std::span<const PortablePackage> packages,
                                                 const PackageImportPlan &plan,
                                                 const std::filesystem::path &output_path, bool overwrite,
                                                 const CancellationToken &cancellation, ProgressSink *progress) {
    try {
        if (auto distinct = require_distinct_source_and_output(target_path, output_path, "package import"); !distinct)
            return std::unexpected{distinct.error()};
        if (!overwrite && std::filesystem::exists(output_path)) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "package import output already exists")};
        }
        if (const auto verified = verify_package_import_plan(plan); !verified)
            return std::unexpected{verified.error()};
        if (!plan.valid())
            return std::unexpected{transaction_error("a conflicting package import plan cannot apply")};
        if (plan.target_kind != MediaKind::sfs && plan.target_kind != MediaKind::fat12_floppy &&
            plan.target_kind != MediaKind::iso9660)
            return std::unexpected{transaction_error("package import target adapter is unsupported")};
        if (packages.size() != plan.package_ids.size())
            return std::unexpected{transaction_error("package import inputs do not match the "
                                                     "planned package count")};
        for (std::size_t index = 0; index < packages.size(); ++index) {
            if (const auto verified = verify_portable_package(packages[index]); !verified)
                return std::unexpected{verified.error()};
            if (packages[index].package_id != plan.package_ids[index]) {
                return std::unexpected{transaction_error("package import input identity differs from the plan")};
            }
        }
        auto snapshot = file_snapshot_id(target_path, cancellation);
        if (!snapshot)
            return std::unexpected{snapshot.error()};
        if (*snapshot != plan.target_snapshot_id)
            return std::unexpected{stale_transaction_error("package import plan is stale for this target")};

        if (plan.target_kind == MediaKind::fat12_floppy) {
            return apply_fat12_package_import(target_path, packages, plan, output_path, overwrite, cancellation,
                                              progress);
        }
        if (plan.target_kind == MediaKind::iso9660) {
            return apply_iso9660_package_import(target_path, packages, plan, output_path, overwrite, cancellation,
                                                progress);
        }

        auto opened = open_transaction_state(target_path, cancellation, progress, true);
        if (!opened)
            return std::unexpected{opened.error()};
        auto state = std::move(*opened);
        auto confirmed_snapshot = file_snapshot_id(target_path, cancellation);
        if (!confirmed_snapshot)
            return std::unexpected{confirmed_snapshot.error()};
        if (*confirmed_snapshot != plan.target_snapshot_id)
            return std::unexpected{transaction_error("package import target changed before "
                                                     "transaction preparation")};

        std::size_t completed{};
        for (const auto &destination : plan.destinations) {
            if (!destination.create)
                continue;
            if (const auto checked = cancellation.check(); !checked)
                return std::unexpected{checked.error()};
            const auto operation_id =
                std::format("package-destination-{}-{}", destination.partition_index, destination.volume_name);
            const InsertVolumeOperation operation{PartitionIndex{destination.partition_index},
                                                  VolumeSpec{destination.volume_name, {}, {}, {}, {}}};
            auto inserted = insert_volume(state, {operation_id, "insert_volume"}, operation, cancellation);
            if (!inserted)
                return std::unexpected{inserted.error()};
            std::vector<std::uint32_t> actual_ids;
            for (const auto id : inserted->inserted_sfs_ids)
                actual_ids.push_back(id.value);
            if (actual_ids != destination.infrastructure_sfs_ids ||
                inserted->allocated_clusters != destination.infrastructure_clusters) {
                return std::unexpected{transaction_error("actual destination volume allocation "
                                                         "differs from the import plan")};
            }
            state.reports.push_back(std::move(*inserted));
            if (progress) {
                progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                                  "creating package destination volume", output_path.string()});
            }
        }
        if (auto grown = grow_package_category_directories(state, plan, cancellation); !grown)
            return std::unexpected{grown.error()};
        if (auto adjusted = apply_existing_sfs_program_assignment_adjustments(state, plan, cancellation); !adjusted)
            return std::unexpected{adjusted.error()};
        std::set<std::pair<std::uint8_t, std::uint32_t>> updated_reused_objects;
        for (const auto &object : plan.objects) {
            if (const auto checked = cancellation.check(); !checked)
                return std::unexpected{checked.error()};
            if (!has_action(object, PackageImportObjectAction::insert)) {
                if (has_action(object, PackageImportObjectAction::reuse) &&
                    has_action(object, PackageImportObjectAction::relocate)) {
                    if (!object.target_sfs_id || (object.object_type != "SMPL" && object.object_type != "SBNK" &&
                                                  object.object_type != "PROG")) {
                        return std::unexpected{
                            transaction_error("planned reused relocation is not a supported fixed object")};
                    }
                    const auto physical_key = std::pair{object.partition_index, *object.target_sfs_id};
                    if (updated_reused_objects.emplace(physical_key).second) {
                        const auto &package = packages[object.package_index];
                        const auto *node = package_node(package, object.node_id);
                        if (node == nullptr)
                            return std::unexpected{transaction_error("package import action node is missing")};
                        auto context = relocation_context(package, plan, object);
                        if (!context)
                            return std::unexpected{context.error()};
                        auto payload = package_internal::relocate_package_node(package, *node, *context);
                        if (!payload)
                            return std::unexpected{payload.error()};
                        auto normalized = normalized_payload_digest(*payload);
                        if (!normalized)
                            return std::unexpected{normalized.error()};
                        if (*normalized != object.normalized_sha256) {
                            return std::unexpected{transaction_error("relocated reused node differs from its "
                                                                     "planned identity")};
                        }
                        const auto partition = state.partitions.find(object.partition_index);
                        if (partition == state.partitions.end()) {
                            return std::unexpected{transaction_error("package import partition is invalid")};
                        }
                        if (auto replaced =
                                replace_fixed_object_payload(state, partition->second, SfsId{*object.target_sfs_id},
                                                             std::move(*payload), cancellation);
                            !replaced) {
                            return std::unexpected{replaced.error()};
                        }
                    }
                }
                ++completed;
                if (progress) {
                    progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                                      "reusing portable package object", output_path.string()});
                }
                continue;
            }
            const auto &package = packages[object.package_index];
            const auto *node = package_node(package, object.node_id);
            if (node == nullptr)
                return std::unexpected{transaction_error("package import action node is missing")};
            auto context = relocation_context(package, plan, object);
            if (!context)
                return std::unexpected{context.error()};
            auto payload = package_internal::relocate_package_node(package, *node, *context);
            if (!payload)
                return std::unexpected{payload.error()};
            auto normalized = normalized_payload_digest(*payload);
            if (!normalized)
                return std::unexpected{normalized.error()};
            if (*normalized != object.normalized_sha256) {
                return std::unexpected{transaction_error(std::format(
                    "relocated package node {} '{}' differs from its planned identity (planned {}, actual {})",
                    object.object_type, object.destination_name, object.normalized_sha256, *normalized))};
            }
            const auto partition = state.partitions.find(object.partition_index);
            if (partition == state.partitions.end() || !object.target_sfs_id)
                return std::unexpected{transaction_error("package import partition or SFS ID is invalid")};
            auto &mutable_partition = partition->second;
            auto allocated = allocate_record(mutable_partition, std::move(*payload), PayloadKind::object,
                                             SfsId{*object.target_sfs_id});
            if (!allocated)
                return std::unexpected{allocated.error()};
            const auto inserted = mutable_partition.inserted.find(SfsId{*object.target_sfs_id});
            if (inserted == mutable_partition.inserted.end())
                return std::unexpected{transaction_error("package insertion did not reserve its record")};
            std::uint64_t payload_clusters{};
            for (const auto &extent : inserted->second.extents)
                payload_clusters += extent.cluster_count;
            if (payload_clusters != object.payload_clusters ||
                inserted->second.continuation_clusters.size() != object.continuation_clusters ||
                allocated->second != object.payload_clusters + object.continuation_clusters) {
                return std::unexpected{transaction_error("actual package allocation differs from the import plan")};
            }
            auto directory =
                volume_category(state, mutable_partition, object.volume_name, object.object_type, cancellation);
            if (!directory)
                return std::unexpected{directory.error()};
            if (auto appended =
                    append_directory_entry(state, mutable_partition, *directory, SfsId{*object.target_sfs_id},
                                           object.destination_name, cancellation);
                !appended) {
                return std::unexpected{appended.error()};
            }
            OperationReport report;
            report.id = object.action_id;
            report.type = "package_insert_object";
            report.partition = PartitionIndex{object.partition_index};
            report.volume_name = object.volume_name;
            report.object_name = object.destination_name;
            report.inserted_sfs_ids = {SfsId{*object.target_sfs_id}};
            report.allocated_clusters = allocated->second;
            state.reports.push_back(std::move(report));
            ++completed;
            if (progress) {
                progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                                  "importing portable package object", output_path.string()});
            }
        }

        std::set<std::tuple<std::uint8_t, SfsId, SfsId>> added_edges;
        for (const auto &owner : plan.objects) {
            const auto &package = packages[owner.package_index];
            for (const auto &edge : package.relationships) {
                if (edge.source_node_id != owner.node_id)
                    continue;
                const auto *target = planned_node(plan, owner, edge.target_node_id);
                if (target == nullptr || !owner.target_sfs_id || !target->target_sfs_id)
                    return std::unexpected{transaction_error("package relationship lacks a planned SFS endpoint")};
                const auto tuple =
                    std::tuple{owner.partition_index, SfsId{*owner.target_sfs_id}, SfsId{*target->target_sfs_id}};
                if (added_edges.emplace(tuple).second &&
                    !std::ranges::contains(state.known_edges,
                                           std::tuple{PartitionIndex{owner.partition_index},
                                                      SfsId{*owner.target_sfs_id}, SfsId{*target->target_sfs_id}})) {
                    state.known_edges.emplace_back(PartitionIndex{owner.partition_index}, SfsId{*owner.target_sfs_id},
                                                   SfsId{*target->target_sfs_id});
                }
            }
        }

        const auto validator = [&](const std::filesystem::path &temporary) {
            return validate_package_result(temporary, packages, plan, cancellation);
        };
        auto published = publish(state, output_path, cancellation, overwrite, validator, progress);
        if (!published) {
            return std::unexpected{published.error()};
        }
        auto output_snapshot = file_snapshot_id(output_path, cancellation);
        if (!output_snapshot)
            return std::unexpected{output_snapshot.error()};
        return PackageImportReport{target_path,
                                   output_path,
                                   plan.plan_id,
                                   plan.target_snapshot_id,
                                   std::move(*output_snapshot),
                                   true,
                                   plan.objects,
                                   plan.program_assignment_adjustments,
                                   plan.allocation,
                                   std::move(*published)};
    } catch (const std::exception &error) {
        return std::unexpected{transaction_error(std::string{"package import callback failed: "} + error.what())};
    } catch (...) {
        return std::unexpected{transaction_error("package import callback failed")};
    }
}

} // namespace axk
