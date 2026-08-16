#include "package_import_support.hpp"

#include "sfs_cluster_allocation.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <queue>
#include <ranges>
#include <tuple>

#include "package_import_opaque_sequences.hpp"

namespace axk::package_import_internal {

Error planner_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Error stale_plan_error(std::string message) {
    return make_error(ErrorCode::transaction_stale, ErrorCategory::transaction, std::move(message));
}

std::string digest_text(std::string_view value) {
    return package_internal::hex_digest(package_internal::sha256(std::as_bytes(std::span{value})));
}

void append_field(std::string &target, std::string_view value) { target += std::format("{}:{};", value.size(), value); }

template <typename Integer> void append_integer(std::string &target, Integer value) {
    append_field(target, std::to_string(value));
}

std::string policy_digest(const PackageImportPolicy &policy) {
    std::string canonical;
    auto renames = policy.renames;
    std::ranges::sort(renames, [](const auto &left, const auto &right) {
        return std::tie(left.package_index, left.node_id, left.destination_name) <
               std::tie(right.package_index, right.node_id, right.destination_name);
    });
    for (const auto &rename : renames) {
        append_integer(canonical, rename.package_index);
        append_field(canonical, rename.node_id);
        append_field(canonical, rename.destination_name);
    }
    auto assignments = policy.program_slot_assignments;
    std::ranges::sort(assignments, [](const auto &left, const auto &right) {
        return std::tie(left.package_index, left.node_id, left.destination_slot) <
               std::tie(right.package_index, right.node_id, right.destination_slot);
    });
    for (const auto &assignment : assignments) {
        append_integer(canonical, assignment.package_index);
        append_field(canonical, assignment.node_id);
        append_integer(canonical, assignment.destination_slot);
    }
    auto opaque_sequences = policy.opaque_sequence_decisions;
    std::ranges::sort(opaque_sequences, [](const auto &left, const auto &right) {
        return std::tie(left.package_index, left.node_id, left.action) <
               std::tie(right.package_index, right.node_id, right.action);
    });
    for (const auto &decision : opaque_sequences) {
        append_integer(canonical, decision.package_index);
        append_field(canonical, decision.node_id);
        append_field(canonical, package_opaque_sequence_action_name(decision.action));
    }
    return digest_text(canonical);
}

bool valid_sfs_name(std::string_view value) {
    return !value.empty() && value.size() <= 16U &&
           std::ranges::all_of(value, [](unsigned char byte) { return byte < 0x80U; });
}

std::uint8_t type_rank(std::string_view type) {
    if (type == "SMPL")
        return 0U;
    if (type == "SBNK")
        return 1U;
    if (type == "SBAC")
        return 2U;
    if (type == "PROG")
        return 3U;
    if (type == "SEQU")
        return 4U;
    return 5U;
}

const PackageNode *node_by_id(const PortablePackage &package, std::string_view node_id) {
    const auto found = std::ranges::find(package.nodes, node_id, &PackageNode::node_id);
    return found == package.nodes.end() ? nullptr : &*found;
}

const PlannedPackageObject *planned_node(const PackageImportPlan &plan, const PlannedPackageObject &owner,
                                         std::string_view node_id) {
    const auto found = std::ranges::find_if(plan.objects, [&](const auto &candidate) {
        return candidate.package_index == owner.package_index && candidate.root_index == owner.root_index &&
               candidate.partition_index == owner.partition_index && candidate.group_name == owner.group_name &&
               candidate.volume_name == owner.volume_name && candidate.raw_group == owner.raw_group &&
               candidate.raw_volume == owner.raw_volume && candidate.node_id == node_id;
    });
    return found == plan.objects.end() ? nullptr : &*found;
}

Result<std::uint8_t> planned_program_number(const PlannedPackageObject &object) {
    unsigned number{};
    const auto [end, error] = std::from_chars(object.destination_name.data(),
                                              object.destination_name.data() + object.destination_name.size(), number);
    if (error != std::errc{} || end != object.destination_name.data() + object.destination_name.size() || number < 1U ||
        number > 128U) {
        return std::unexpected{planner_error("destination Program names must be decimal slots 001 through 128")};
    }
    if (object.destination_name != std::format("{:03}", number)) {
        return std::unexpected{planner_error("destination Program names must be decimal slots 001 through 128")};
    }
    return static_cast<std::uint8_t>(number);
}

Result<package_internal::PackageNodeRelocationContext>
relocation_context(const PortablePackage &package, const PackageImportPlan &plan, const PlannedPackageObject &owner) {
    package_internal::PackageNodeRelocationContext context;
    context.destination_name = owner.destination_name;
    context.wave_data_reference_value = owner.target_wave_data_reference_value;
    context.linked_program_numbers = owner.target_program_numbers;
    context.sample_bank_member = owner.target_sample_bank_member;
    for (const auto &adjustment : plan.program_assignment_adjustments) {
        if (adjustment.origin == PackageProgramAssignmentOrigin::imported_program && adjustment.action_id &&
            *adjustment.action_id == owner.action_id) {
            context.cleared_program_assignment_ordinals.push_back(adjustment.assignment_ordinal);
        }
    }
    for (const auto &edge : package.relationships) {
        if (edge.source_node_id != owner.node_id)
            continue;
        const auto *target = planned_node(plan, owner, edge.target_node_id);
        if (target == nullptr) {
            return std::unexpected{planner_error("package relationship target is absent from the "
                                                 "planned destination")};
        }
        context.edge_target_names.emplace(edge.edge_id, target->destination_name);
        if (target->target_wave_data_reference_value)
            context.edge_target_reference_values.emplace(edge.edge_id, *target->target_wave_data_reference_value);
    }
    return context;
}

std::vector<const PackageNode *> root_closure(const PortablePackage &package, std::size_t root_index,
                                              std::size_t package_index, const PackageImportPolicy &policy) {
    std::map<std::string, std::vector<std::string>, std::less<>> children;
    for (const auto &relationship : package.relationships)
        children[relationship.source_node_id].push_back(relationship.target_node_id);
    std::set<std::string, std::less<>> visited;
    std::queue<std::string> pending;
    for (const auto &node_id : package.roots[root_index].node_ids)
        pending.push(node_id);
    while (!pending.empty()) {
        auto node_id = std::move(pending.front());
        pending.pop();
        if (!visited.emplace(node_id).second)
            continue;
        if (const auto found = children.find(node_id); found != children.end()) {
            for (const auto &child : found->second)
                pending.push(child);
        }
    }
    std::vector<const PackageNode *> result;
    result.reserve(visited.size());
    for (const auto &node_id : visited) {
        if (const auto *node = node_by_id(package, node_id);
            node != nullptr && !skip_opaque_sequence(package, policy, package_index, *node)) {
            result.push_back(node);
        }
    }
    return result;
}

std::map<DestinationKey, SfsVolume> sfs_volumes(const Container &container) {
    std::map<DestinationKey, SfsVolume> result;
    for (const auto &partition : container.partitions()) {
        std::map<std::uint32_t, const IndexRecord *> directories;
        for (const auto &record : partition.records) {
            if (record.directory_id)
                directories.emplace(record.directory_id->value, &record);
        }
        const auto located_root = locate_partition_root_record(partition);
        if (!located_root)
            continue;
        const auto root_record = std::ranges::find(partition.records, *located_root, &IndexRecord::sfs_id);
        if (root_record == partition.records.end())
            continue;
        const auto *root = &*root_record;
        for (const auto &entry : root->directory_entries) {
            if (entry.name == "." || entry.name == ".." || is_partition_support_root_entry(entry.name))
                continue;
            const auto found = directories.find(entry.link_id.value);
            if (found == directories.end())
                continue;
            SfsVolume volume{&partition, found->second, {}};
            for (const auto &category_entry : found->second->directory_entries) {
                const auto category = directories.find(category_entry.link_id.value);
                if (category_entry.name != "." && category_entry.name != ".." && category != directories.end()) {
                    volume.categories.emplace(category_entry.name, category->second);
                }
            }
            result.emplace(DestinationKey{partition.index.value, entry.name}, std::move(volume));
        }
    }
    return result;
}

std::vector<ExistingObject> existing_objects(const ObjectCatalog &catalog) {
    std::vector<ExistingObject> result;
    result.reserve(catalog.objects.size());
    for (const auto &object : catalog.objects) {
        ExistingObject item{&object, {}, {}, {}};
        const auto profile = package_internal::build_relocation_profile(object.object, object.raw_payload);
        if (profile) {
            item.normalized_sha256 =
                package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
        }
        if (const auto *wave_data = std::get_if<CurrentSmpl>(&object.object.payload);
            wave_data != nullptr && wave_data->wave_data_reference_value.value != 0U) {
            item.wave_data_reference_value = wave_data->wave_data_reference_value.value;
        }
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<ExistingObject> retained_existing_objects(std::span<const ObjectSnapshot *const> objects) {
    std::vector<ExistingObject> result;
    result.reserve(objects.size());
    for (const auto *object : objects) {
        if (object == nullptr)
            continue;
        ExistingObject item{object, {}, {}, {}};
        if (const auto *wave_data = std::get_if<CurrentSmpl>(&object->object.payload);
            wave_data != nullptr && wave_data->wave_data_reference_value.value != 0U) {
            item.wave_data_reference_value = wave_data->wave_data_reference_value.value;
        }
        result.push_back(std::move(item));
    }
    return result;
}

std::span<const std::byte> existing_payload(const ExistingObject &object) {
    if (object.loaded_payload)
        return *object.loaded_payload;
    return object.snapshot->raw_payload;
}

Result<void> ensure_existing_identity(ExistingObject &object, const Container &container,
                                      package_import_internal::RetainedPackageImportStats *stats,
                                      const CancellationToken &cancellation) {
    if (object.normalized_sha256)
        return {};
    if (object.snapshot->raw_payload.empty()) {
        auto payload = container.read_record_data(object.snapshot->partition, object.snapshot->sfs_id,
                                                  64U * 1024U * 1024U, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        object.loaded_payload.emplace(std::move(*payload));
        if (stats != nullptr) {
            if (object.loaded_payload->size() >
                std::numeric_limits<std::uint64_t>::max() - stats->target_payload_bytes_read) {
                return std::unexpected{planner_error("target payload diagnostic byte count overflow")};
            }
            stats->target_payload_bytes_read += object.loaded_payload->size();
            ++stats->target_payload_objects_read;
        }
    }
    const auto payload = existing_payload(object);
    const auto profile = package_internal::build_relocation_profile(object.snapshot->object, payload);
    if (!profile)
        return std::unexpected{profile.error()};
    object.normalized_sha256 = package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
    return {};
}

void add_conflict(PackageImportPlan &plan, std::string code, std::string message,
                  const PackageRootDestination *destination, const PortablePackage *package, const PackageNode *node) {
    PackageImportConflict conflict;
    conflict.code = std::move(code);
    conflict.message = std::move(message);
    if (destination != nullptr) {
        conflict.package_index = destination->package_index;
        conflict.root_index = destination->root_index;
        conflict.partition_index = destination->partition_index;
        conflict.group_name = destination->group_name;
        conflict.volume_name = destination->volume_name;
        conflict.raw_group = destination->raw_group;
        conflict.raw_volume = destination->raw_volume;
    }
    if (package != nullptr)
        conflict.package_id = package->package_id;
    if (node != nullptr)
        conflict.node_id = node->node_id;
    plan.conflicts.push_back(std::move(conflict));
}

Result<std::string> projected_normalized_sha256(const PortablePackage &package, const PackageNode &node,
                                                const std::map<std::string, std::string, std::less<>> &names) {
    package_internal::PackageNodeRelocationContext context;
    context.destination_name = names.at(node.node_id);
    for (const auto &edge : package.relationships) {
        if (edge.source_node_id != node.node_id)
            continue;
        const auto target = names.find(edge.target_node_id);
        if (target == names.end()) {
            return std::unexpected{planner_error("package relationship target is absent from its "
                                                 "destination closure")};
        }
        context.edge_target_names.emplace(edge.edge_id, target->second);
    }
    return projected_normalized_sha256(package, node, context);
}

Result<std::string> projected_normalized_sha256(const PortablePackage &package, const PackageNode &node,
                                                const package_internal::PackageNodeRelocationContext &context) {
    auto projected = package_internal::project_package_node_names(package, node, context);
    if (!projected)
        return std::unexpected{projected.error()};
    auto decoded = package_internal::decode_package_object(*projected);
    if (!decoded)
        return std::unexpected{decoded.error()};
    auto profile = package_internal::build_relocation_profile(*decoded, *projected);
    if (!profile)
        return std::unexpected{profile.error()};
    return package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
}

PartitionCapacity partition_capacity(const Partition &partition, const ObjectCatalog &catalog) {
    PartitionCapacity result;
    result.partition = &partition;
    result.index_block_count = static_cast<std::uint64_t>(partition.directory_index_span_clusters) *
                               partition.sectors_per_cluster * 512U / 1024U;
    result.total_record_slots = result.index_block_count * result.records_per_index_block;
    result.reserved_record_slots = std::min<std::uint64_t>(3U, result.total_record_slots);
    result.allocatable_record_slots = result.total_record_slots - result.reserved_record_slots;
    std::set<std::uint32_t> used_ids;
    for (const auto &record : partition.records) {
        used_ids.insert(record.sfs_id.value);
        for (const auto &extent : record.extents) {
            for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
                 ++cluster) {
                result.used_clusters.insert(cluster);
            }
        }
        result.used_clusters.insert(record.continuation_clusters.begin(), record.continuation_clusters.end());
    }
    for (std::uint32_t id = 3U; id < result.total_record_slots; ++id) {
        if (!used_ids.contains(id))
            result.free_ids.push_back(id);
    }
    result.used_record_slots = result.allocatable_record_slots - result.free_ids.size();
    for (const auto &object : catalog.objects) {
        if (object.partition != partition.index)
            continue;
        if (const auto *wave_data = std::get_if<CurrentSmpl>(&object.object.payload);
            wave_data != nullptr && wave_data->wave_data_reference_value.value != 0U) {
            result.used_wave_data_reference_values.insert(wave_data->wave_data_reference_value.value);
        }
    }
    return result;
}

PartitionCapacity partition_capacity(const Partition &partition,
                                     std::span<const ObjectSnapshot *const> catalog_objects) {
    PartitionCapacity result;
    result.partition = &partition;
    result.index_block_count = static_cast<std::uint64_t>(partition.directory_index_span_clusters) *
                               partition.sectors_per_cluster * 512U / 1024U;
    result.total_record_slots = result.index_block_count * result.records_per_index_block;
    result.reserved_record_slots = std::min<std::uint64_t>(3U, result.total_record_slots);
    result.allocatable_record_slots = result.total_record_slots - result.reserved_record_slots;
    std::set<std::uint32_t> used_ids;
    for (const auto &record : partition.records) {
        used_ids.insert(record.sfs_id.value);
        for (const auto &extent : record.extents) {
            for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
                 ++cluster) {
                result.used_clusters.insert(cluster);
            }
        }
        result.used_clusters.insert(record.continuation_clusters.begin(), record.continuation_clusters.end());
    }
    for (std::uint32_t id = 3U; id < result.total_record_slots; ++id) {
        if (!used_ids.contains(id))
            result.free_ids.push_back(id);
    }
    result.used_record_slots = result.allocatable_record_slots - result.free_ids.size();
    for (const auto *object : catalog_objects) {
        if (object == nullptr || object->partition != partition.index)
            continue;
        if (const auto *wave_data = std::get_if<CurrentSmpl>(&object->object.payload);
            wave_data != nullptr && wave_data->wave_data_reference_value.value != 0U) {
            result.used_wave_data_reference_values.insert(wave_data->wave_data_reference_value.value);
        }
    }
    return result;
}

std::vector<Extent> cluster_extents(std::span<const std::uint32_t> clusters) {
    std::vector<Extent> result;
    for (const auto cluster : clusters) {
        if (!result.empty() && result.back().cluster_offset + result.back().cluster_count == cluster) {
            ++result.back().cluster_count;
            result.back().byte_count += 1024U;
        } else {
            result.push_back({cluster, 1U, 1024U});
        }
    }
    return result;
}

std::vector<Extent> merged_extents(std::span<const Extent> existing, std::span<const Extent> added) {
    std::vector<Extent> result;
    result.reserve(existing.size() + added.size());
    result.insert(result.end(), existing.begin(), existing.end());
    result.insert(result.end(), added.begin(), added.end());
    std::ranges::sort(result, {}, &Extent::cluster_offset);
    std::vector<Extent> merged;
    for (const auto &extent : result) {
        if (!merged.empty() && merged.back().cluster_offset + merged.back().cluster_count == extent.cluster_offset) {
            merged.back().cluster_count += extent.cluster_count;
            merged.back().byte_count += extent.byte_count;
        } else {
            merged.push_back(extent);
        }
    }
    return merged;
}

std::optional<ClusterReservation> reserve_clusters(PartitionCapacity &capacity, std::uint32_t payload_cluster_count) {
    const auto first = capacity.partition->directory_index_cluster + capacity.partition->directory_index_span_clusters;
    auto selected = detail::select_sfs_payload_clusters(
        first, capacity.partition->cluster_count, payload_cluster_count,
        [&](const std::uint32_t cluster) { return capacity.used_clusters.contains(cluster); });
    if (!selected)
        return std::nullopt;
    const auto extents = cluster_extents(*selected);
    const std::set selected_set(selected->begin(), selected->end());
    constexpr std::size_t extents_per_list_cluster = (1024U - 12U) / 12U;
    const auto list_count =
        extents.size() <= 4U ? 0U : (extents.size() + extents_per_list_cluster - 1U) / extents_per_list_cluster;
    std::vector<std::uint32_t> selected_lists;
    for (std::uint32_t cluster = first;
         cluster < capacity.partition->cluster_count && selected_lists.size() < list_count; ++cluster) {
        if (!capacity.used_clusters.contains(cluster) && !selected_set.contains(cluster))
            selected_lists.push_back(cluster);
    }
    if (selected_lists.size() != list_count)
        return std::nullopt;
    capacity.used_clusters.insert(selected->begin(), selected->end());
    capacity.used_clusters.insert(selected_lists.begin(), selected_lists.end());
    return ClusterReservation{payload_cluster_count, static_cast<std::uint64_t>(list_count), extents};
}

std::optional<ClusterReservation> reserve_directory_growth(PartitionCapacity &capacity,
                                                           std::span<const Extent> existing_extents,
                                                           std::size_t existing_continuation_clusters,
                                                           std::uint32_t additional_payload_clusters) {
    if (additional_payload_clusters == 0U)
        return ClusterReservation{};
    const auto first = capacity.partition->directory_index_cluster + capacity.partition->directory_index_span_clusters;
    auto selected = detail::select_sfs_payload_clusters(
        first, capacity.partition->cluster_count, additional_payload_clusters,
        [&](const std::uint32_t cluster) { return capacity.used_clusters.contains(cluster); });
    if (!selected)
        return std::nullopt;
    const auto added_extents = cluster_extents(*selected);
    const auto extents = merged_extents(existing_extents, added_extents);
    constexpr std::size_t extents_per_list_cluster = (1024U - 12U) / 12U;
    const auto required_lists =
        extents.size() <= 4U ? 0U : (extents.size() + extents_per_list_cluster - 1U) / extents_per_list_cluster;
    if (required_lists < existing_continuation_clusters)
        return std::nullopt;
    const auto additional_lists = required_lists - existing_continuation_clusters;
    const std::set selected_set(selected->begin(), selected->end());
    std::vector<std::uint32_t> selected_lists;
    for (std::uint32_t cluster = first;
         cluster < capacity.partition->cluster_count && selected_lists.size() < additional_lists; ++cluster) {
        if (!capacity.used_clusters.contains(cluster) && !selected_set.contains(cluster))
            selected_lists.push_back(cluster);
    }
    if (selected_lists.size() != additional_lists)
        return std::nullopt;
    capacity.used_clusters.insert(selected->begin(), selected->end());
    capacity.used_clusters.insert(selected_lists.begin(), selected_lists.end());
    return ClusterReservation{additional_payload_clusters, static_cast<std::uint64_t>(additional_lists), extents};
}

std::uint64_t remaining_clusters(const PartitionCapacity &capacity) {
    const auto first = capacity.partition->directory_index_cluster + capacity.partition->directory_index_span_clusters;
    std::uint64_t result{};
    for (std::uint32_t cluster = first; cluster < capacity.partition->cluster_count; ++cluster) {
        if (!capacity.used_clusters.contains(cluster))
            ++result;
    }
    return result;
}

std::string action_identity(const Candidate &candidate) {
    std::string source;
    append_field(source, candidate.package->package_id);
    append_integer(source, candidate.destination->package_index);
    append_integer(source, candidate.destination->root_index);
    append_field(source, candidate.node->node_id);
    append_integer(source, *candidate.destination->partition_index);
    append_field(source, candidate.destination->group_name);
    append_field(source, candidate.destination->volume_name);
    append_field(source, candidate.destination->raw_group);
    append_field(source, candidate.destination->raw_volume);
    append_field(source, candidate.destination_name);
    return digest_text(source);
}

void mark_conflict(PlannedPackageObject &object) {
    if (!std::ranges::contains(object.actions, PackageImportObjectAction::conflict))
        object.actions.push_back(PackageImportObjectAction::conflict);
}

bool valid_iso_raw_group(std::string_view value) {
    return !value.empty() && value.size() <= 8U && std::ranges::all_of(value, [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
    });
}

bool valid_iso_raw_volume(std::string_view value) {
    return value.size() == 4U && value[0] == 'F' && value != "F000" &&
           std::ranges::all_of(value.substr(1),
                               [](unsigned char character) { return character >= '0' && character <= '9'; });
}

std::optional<std::pair<std::string, std::string>> iso_raw_scope(const ObjectSnapshot &snapshot) {
    if (!snapshot.placement)
        return std::nullopt;
    const auto &path = snapshot.placement->container_directory;
    const auto separator = path.find('/');
    if (separator == std::string::npos || path.find('/', separator + 1U) != std::string::npos)
        return std::nullopt;
    return std::pair{path.substr(0, separator), path.substr(separator + 1U)};
}

} // namespace axk::package_import_internal
