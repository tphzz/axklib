#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

Result<void> grow_package_category_directories(TransactionState &state, const PackageImportPlan &plan,
                                               const CancellationToken &cancellation) {
    using CategoryKey = std::tuple<std::uint8_t, std::string, std::string>;
    using DestinationKey = std::pair<std::uint8_t, std::string>;
    std::map<CategoryKey, std::size_t> insertions;
    for (const auto &object : plan.objects) {
        if (has_action(object, PackageImportObjectAction::insert) &&
            !has_action(object, PackageImportObjectAction::conflict)) {
            ++insertions[{object.partition_index, object.volume_name, object.object_type}];
        }
    }
    std::map<DestinationKey, std::pair<std::uint64_t, std::uint64_t>> actual;
    for (const auto &[key, count] : insertions) {
        const auto &[partition_index, volume_name, category_name] = key;
        const auto partition = state.partitions.find(partition_index);
        if (partition == state.partitions.end())
            return std::unexpected{transaction_error("package directory growth partition is unavailable")};
        auto directory = volume_category(state, partition->second, volume_name, category_name, cancellation);
        if (!directory)
            return std::unexpected{directory.error()};
        auto payload = current_payload(state, partition->second, *directory, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        const auto required_size = static_cast<std::uint64_t>(payload->size()) + count * 32U;
        auto growth = grow_directory_capacity(state, partition->second, *directory, required_size, cancellation);
        if (!growth)
            return std::unexpected{growth.error()};
        auto &totals = actual[{partition_index, volume_name}];
        totals.first += growth->first;
        totals.second += growth->second;
    }
    for (const auto &allocation : plan.allocation) {
        const auto found = actual.find({allocation.partition_index, allocation.volume_name});
        const auto payload_clusters = found == actual.end() ? 0U : found->second.first;
        const auto continuation_clusters = found == actual.end() ? 0U : found->second.second;
        if (payload_clusters != allocation.directory_growth_clusters ||
            continuation_clusters != allocation.directory_continuation_clusters) {
            return std::unexpected{transaction_error("actual package directory growth differs from the import plan")};
        }
    }
    return {};
}

Result<TransactionState> prepare_sfs_package_import_state(std::shared_ptr<const RandomAccessReader> source,
                                                          const std::filesystem::path &source_path,
                                                          std::span<const PortablePackage> packages,
                                                          const PackageImportPlan &plan,
                                                          std::optional<std::string_view> verified_source_snapshot_id,
                                                          const CancellationToken &cancellation,
                                                          ProgressSink *progress) {
    std::string source_snapshot_id;
    if (verified_source_snapshot_id) {
        source_snapshot_id = *verified_source_snapshot_id;
    } else {
        auto digest = package_internal::sha256_reader(*source, cancellation);
        if (!digest)
            return std::unexpected{digest.error()};
        source_snapshot_id = package_internal::hex_digest(*digest);
    }
    if (source_snapshot_id != plan.target_snapshot_id)
        return std::unexpected{stale_transaction_error("package import plan is stale for this target")};
    auto opened = open_transaction_state(std::move(source), source_path, cancellation, progress, true);
    if (!opened)
        return std::unexpected{opened.error()};
    auto state = std::move(*opened);
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
            return std::unexpected{
                transaction_error("actual destination volume allocation differs from the import plan")};
        }
        state.reports.push_back(std::move(*inserted));
        if (progress) {
            progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                              "creating package destination volume", text::path_to_utf8(source_path)});
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
                if (!object.target_sfs_id || (object.object_type != "SBNK" && object.object_type != "PROG")) {
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
                        return std::unexpected{
                            transaction_error("relocated reused node differs from its planned identity")};
                    }
                    const auto partition = state.partitions.find(object.partition_index);
                    if (partition == state.partitions.end())
                        return std::unexpected{transaction_error("package import partition is invalid")};
                    if (auto replaced = replace_fixed_object_payload(
                            state, partition->second, SfsId{*object.target_sfs_id}, std::move(*payload), cancellation);
                        !replaced) {
                        return std::unexpected{replaced.error()};
                    }
                }
            }
            ++completed;
            if (progress) {
                progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                                  "reusing portable package object", text::path_to_utf8(source_path)});
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
            return std::unexpected{transaction_error(
                std::format("relocated package node {} '{}' differs from its planned identity (planned {}, actual {})",
                            object.object_type, object.destination_name, object.normalized_sha256, *normalized))};
        }
        const auto partition = state.partitions.find(object.partition_index);
        if (partition == state.partitions.end() || !object.target_sfs_id)
            return std::unexpected{transaction_error("package import partition or SFS ID is invalid")};
        auto &mutable_partition = partition->second;
        auto allocated =
            allocate_record(mutable_partition, std::move(*payload), PayloadKind::object, SfsId{*object.target_sfs_id});
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
        if (auto appended = append_directory_entry(state, mutable_partition, *directory, SfsId{*object.target_sfs_id},
                                                   object.destination_name, cancellation);
            !appended) {
            return std::unexpected{appended.error()};
        }
        ++completed;
        if (progress) {
            progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                              "importing portable package object", text::path_to_utf8(source_path)});
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
                                       std::tuple{PartitionIndex{owner.partition_index}, SfsId{*owner.target_sfs_id},
                                                  SfsId{*target->target_sfs_id}})) {
                state.known_edges.emplace_back(PartitionIndex{owner.partition_index}, SfsId{*owner.target_sfs_id},
                                               SfsId{*target->target_sfs_id});
            }
        }
    }
    return state;
}

class PatchedReader final : public RandomAccessReader {
  public:
    PatchedReader(std::shared_ptr<const RandomAccessReader> source, std::span<const detail::AlterationPatch> patches)
        : source_{std::move(source)}, patches_{patches} {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return source_->size(); }

    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (auto read = source_->read_exact_at(offset, destination); !read)
            return read;
        const auto end = offset + destination.size();
        for (const auto &patch : patches_) {
            const auto patch_end = patch.offset + patch.replacement.size();
            const auto overlap_begin = std::max(offset, patch.offset);
            const auto overlap_end = std::min(end, patch_end);
            if (overlap_begin >= overlap_end)
                continue;
            const auto source_offset = static_cast<std::size_t>(overlap_begin - patch.offset);
            const auto destination_offset = static_cast<std::size_t>(overlap_begin - offset);
            const auto count = static_cast<std::size_t>(overlap_end - overlap_begin);
            std::ranges::copy(std::span{patch.replacement}.subspan(source_offset, count),
                              destination.begin() + static_cast<std::ptrdiff_t>(destination_offset));
        }
        return {};
    }

  private:
    std::shared_ptr<const RandomAccessReader> source_;
    std::span<const detail::AlterationPatch> patches_;
};

std::shared_ptr<const RandomAccessReader> patched_reader(std::shared_ptr<const RandomAccessReader> source,
                                                         std::span<const detail::AlterationPatch> patches) {
    return std::make_shared<PatchedReader>(std::move(source), patches);
}

Result<void> validate_package_result(std::shared_ptr<const RandomAccessReader> reader,
                                     const std::filesystem::path &source_path,
                                     std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                                     const CancellationToken &cancellation) {
    auto media = open_media(std::move(reader), source_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::sfs)
        return std::unexpected{transaction_error("package result is not an SFS image")};
    auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    const auto graph = build_relationship_graph(*catalog);
    std::map<std::string, const ObjectSnapshot *, std::less<>> actual_by_action;
    for (const auto &object : plan.objects) {
        std::vector<const ObjectSnapshot *> matches;
        for (const auto &snapshot : catalog->objects) {
            if (!object.target_sfs_id || !snapshot.placement || snapshot.partition.value != object.partition_index ||
                snapshot.sfs_id.value != *object.target_sfs_id ||
                snapshot.placement->volume_name != object.volume_name ||
                snapshot.object.header.raw_type != object.object_type ||
                snapshot.object.header.name != object.destination_name) {
                continue;
            }
            matches.push_back(&snapshot);
        }
        if (matches.size() != 1U) {
            return std::unexpected{transaction_error("post-write package object does not match "
                                                     "its planned placement")};
        }
        auto normalized = normalized_payload_digest(matches.front()->raw_payload);
        if (!normalized)
            return std::unexpected{normalized.error()};
        if (*normalized != object.normalized_sha256) {
            return std::unexpected{transaction_error("post-write package object changed normalized identity")};
        }
        if (object.target_wave_data_reference_value) {
            const auto *wave_data = std::get_if<CurrentSmpl>(&matches.front()->object.payload);
            if (wave_data == nullptr ||
                wave_data->wave_data_reference_value.value != *object.target_wave_data_reference_value) {
                return std::unexpected{
                    transaction_error("post-write SMPL reference value differs from the import plan")};
            }
        }
        if (object.object_type == "SBNK") {
            const auto *sample = std::get_if<CurrentSbnk>(&matches.front()->object.payload);
            if (sample == nullptr || sample->linked_program_numbers != object.target_program_numbers ||
                (((sample->sample_flags & 1U) != 0U) != object.target_sample_bank_member)) {
                return std::unexpected{transaction_error("post-write SBNK graph metadata differs "
                                                         "from the import plan")};
            }
        }
        actual_by_action.emplace(object.action_id, matches.front());
    }

    std::map<std::string, std::vector<const PackageProgramAssignmentAdjustment *>, std::less<>> adjustments_by_owner;
    for (const auto &adjustment : plan.program_assignment_adjustments) {
        const auto owner = adjustment.origin == PackageProgramAssignmentOrigin::imported_program
                               ? "action:" + *adjustment.action_id
                               : "existing:" + *adjustment.existing_object_key;
        adjustments_by_owner[owner].push_back(&adjustment);
    }
    for (const auto &[owner, adjustments] : adjustments_by_owner) {
        const ObjectSnapshot *snapshot{};
        if (owner.starts_with("action:")) {
            const auto found = actual_by_action.find(owner.substr(7U));
            if (found != actual_by_action.end())
                snapshot = found->second;
        } else {
            const auto found = std::ranges::find(catalog->objects, owner.substr(9U), &ObjectSnapshot::key);
            if (found != catalog->objects.end())
                snapshot = &*found;
        }
        if (snapshot == nullptr)
            return std::unexpected{transaction_error("adjusted Program is absent after package import")};
        if (auto validated = validate_cleared_program_assignment_adjustments(*snapshot, adjustments); !validated)
            return std::unexpected{validated.error()};
    }

    for (const auto &owner : plan.objects) {
        const auto &package = packages[owner.package_index];
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != owner.node_id)
                continue;
            const auto *target_plan = planned_node(plan, owner, edge.target_node_id);
            if (target_plan == nullptr)
                return std::unexpected{transaction_error("post-write edge has no target action")};
            const auto source = actual_by_action.find(owner.action_id);
            const auto target = actual_by_action.find(target_plan->action_id);
            if (source == actual_by_action.end() || target == actual_by_action.end())
                return std::unexpected{transaction_error("post-write edge endpoint is missing")};
            const auto relationships = graph.children(source->second->key);
            const auto matched = std::ranges::find_if(relationships, [&](const Relationship *actual) {
                return actual->type == edge.role && actual->quality == RelationshipQuality::known &&
                       actual->target_key && *actual->target_key == target->second->key;
            });
            if (matched == relationships.end()) {
                return std::unexpected{
                    transaction_error(std::format("post-write {} relationship from {} to {} "
                                                  "differs from the package plan",
                                                  edge.role, owner.destination_name, target_plan->destination_name))};
            }
        }
    }
    return {};
}

Result<void> validate_package_result(const std::filesystem::path &temporary, std::span<const PortablePackage> packages,
                                     const PackageImportPlan &plan, const CancellationToken &cancellation) {
    auto reader = FileReader::open(temporary);
    if (!reader)
        return std::unexpected{reader.error()};
    return validate_package_result(std::move(*reader), temporary, packages, plan, cancellation);
}

Result<std::string> file_snapshot_id(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected{digest.error()};
    return package_internal::hex_digest(*digest);
}

} // namespace axk::alteration_internal
