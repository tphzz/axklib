#include "axklib/package.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <map>
#include <queue>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/catalog.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/package_relocation.hpp"

namespace axk {
namespace {

using DestinationKey = std::pair<std::uint8_t, std::string>;

Error planner_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
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
    context.smpl_link_id = owner.target_link_id;
    context.linked_program_numbers = owner.target_program_numbers;
    context.grouped = owner.target_grouped;
    for (const auto &edge : package.relationships) {
        if (edge.source_node_id != owner.node_id)
            continue;
        const auto *target = planned_node(plan, owner, edge.target_node_id);
        if (target == nullptr) {
            return std::unexpected{planner_error("package relationship target is absent from the "
                                                 "planned destination")};
        }
        context.edge_target_names.emplace(edge.edge_id, target->destination_name);
        if (target->target_link_id)
            context.edge_target_link_ids.emplace(edge.edge_id, *target->target_link_id);
    }
    return context;
}

std::vector<const PackageNode *> root_closure(const PortablePackage &package, std::size_t root_index) {
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
        if (const auto *node = node_by_id(package, node_id); node != nullptr)
            result.push_back(node);
    }
    return result;
}

struct SfsVolume {
    const Partition *partition{};
    const IndexRecord *directory{};
    std::map<std::string, const IndexRecord *, std::less<>> categories;
};

std::map<DestinationKey, SfsVolume> sfs_volumes(const Container &container) {
    std::map<DestinationKey, SfsVolume> result;
    for (const auto &partition : container.partitions()) {
        std::map<std::uint32_t, const IndexRecord *> directories;
        for (const auto &record : partition.records) {
            if (record.directory_id)
                directories.emplace(record.directory_id->value, &record);
        }
        const IndexRecord *root{};
        for (const auto &[id, directory] : directories) {
            if (directory->parent_directory_id && directory->parent_directory_id->value == id) {
                root = directory;
                break;
            }
        }
        if (root == nullptr || !root->directory_id)
            continue;
        for (const auto &entry : root->directory_entries) {
            if (entry.name == "." || entry.name == "..")
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

struct ExistingObject {
    const ObjectSnapshot *snapshot{};
    std::optional<std::string> normalized_sha256;
    std::optional<std::uint32_t> link_id;
};

std::vector<ExistingObject> existing_objects(const ObjectCatalog &catalog) {
    std::vector<ExistingObject> result;
    result.reserve(catalog.objects.size());
    for (const auto &object : catalog.objects) {
        ExistingObject item{&object, {}, {}};
        const auto profile = package_internal::build_relocation_profile(object.object, object.raw_payload);
        if (profile) {
            item.normalized_sha256 =
                package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
        }
        if (const auto *sample = std::get_if<CurrentSmpl>(&object.object.payload);
            sample != nullptr && sample->link_id.value != 0U) {
            item.link_id = sample->link_id.value;
        }
        result.push_back(std::move(item));
    }
    return result;
}

void add_conflict(PackageImportPlan &plan, std::string code, std::string message,
                  const PackageRootDestination *destination = nullptr, const PortablePackage *package = nullptr,
                  const PackageNode *node = nullptr) {
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

struct Candidate {
    const PortablePackage *package{};
    const PackageNode *node{};
    const PackageRootDestination *destination{};
    std::string destination_name;
    std::string projected_normalized_sha256;
};

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
    auto projected = package_internal::project_package_node_names(package, node, context);
    if (!projected)
        return std::unexpected{projected.error()};
    auto decoded = decode_object(*projected);
    if (!decoded)
        return std::unexpected{decoded.error()};
    auto profile = package_internal::build_relocation_profile(*decoded, *projected);
    if (!profile)
        return std::unexpected{profile.error()};
    return package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
}

struct PartitionCapacity {
    const Partition *partition{};
    std::vector<std::uint32_t> free_ids;
    std::set<std::uint32_t> used_clusters;
    std::set<std::uint32_t> used_link_ids;
    std::size_t next_id{};
    std::uint32_t next_link_id{0x016b1dbcU};
};

PartitionCapacity partition_capacity(const Partition &partition, const ObjectCatalog &catalog) {
    PartitionCapacity result;
    result.partition = &partition;
    const auto capacity = (static_cast<std::uint64_t>(partition.directory_index_span_clusters) *
                           partition.sectors_per_cluster * 512U / 1024U) *
                          14U;
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
    for (std::uint32_t id = 3U; id < capacity; ++id) {
        if (!used_ids.contains(id))
            result.free_ids.push_back(id);
    }
    for (const auto &object : catalog.objects) {
        if (object.partition != partition.index)
            continue;
        if (const auto *sample = std::get_if<CurrentSmpl>(&object.object.payload);
            sample != nullptr && sample->link_id.value != 0U) {
            result.used_link_ids.insert(sample->link_id.value);
        }
    }
    return result;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> reserve_clusters(PartitionCapacity &capacity,
                                                                        std::uint32_t payload_cluster_count) {
    std::vector<std::uint32_t> selected;
    const auto first = capacity.partition->directory_index_cluster + capacity.partition->directory_index_span_clusters;
    for (std::uint32_t cluster = first;
         cluster < capacity.partition->cluster_count && selected.size() < payload_cluster_count; ++cluster) {
        if (!capacity.used_clusters.contains(cluster))
            selected.push_back(cluster);
    }
    if (selected.size() != payload_cluster_count)
        return std::nullopt;
    std::size_t extent_count{};
    std::optional<std::uint32_t> previous;
    for (const auto cluster : selected) {
        if (!previous || *previous + 1U != cluster)
            ++extent_count;
        previous = cluster;
    }
    const std::set selected_set(selected.begin(), selected.end());
    constexpr std::size_t extents_per_list_cluster = (1024U - 12U) / 12U;
    const auto list_count =
        extent_count <= 4U ? 0U : (extent_count + extents_per_list_cluster - 1U) / extents_per_list_cluster;
    std::vector<std::uint32_t> selected_lists;
    for (std::uint32_t cluster = first;
         cluster < capacity.partition->cluster_count && selected_lists.size() < list_count; ++cluster) {
        if (!capacity.used_clusters.contains(cluster) && !selected_set.contains(cluster))
            selected_lists.push_back(cluster);
    }
    if (selected_lists.size() != list_count)
        return std::nullopt;
    capacity.used_clusters.insert(selected.begin(), selected.end());
    capacity.used_clusters.insert(selected_lists.begin(), selected_lists.end());
    return std::pair{payload_cluster_count, static_cast<std::uint64_t>(list_count)};
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

std::string plan_identity(const PackageImportPlan &plan) {
    std::string source;
    append_field(source, plan.schema_version);
    append_integer(source, static_cast<std::uint8_t>(plan.target_kind));
    append_field(source, plan.target_snapshot_id);
    append_field(source, plan.policy_digest);
    for (const auto &package_id : plan.package_ids)
        append_field(source, package_id);
    for (const auto &destination : plan.destinations) {
        append_integer(source, destination.partition_index);
        append_field(source, destination.group_name);
        append_field(source, destination.volume_name);
        append_field(source, destination.raw_group);
        append_field(source, destination.raw_volume);
        append_integer(source, destination.create);
        for (const auto id : destination.infrastructure_sfs_ids)
            append_integer(source, id);
        append_integer(source, destination.infrastructure_clusters);
        append_integer(source, destination.root_directory_growth_bytes);
    }
    for (const auto &object : plan.objects) {
        append_field(source, object.action_id);
        append_integer(source, object.package_index);
        append_integer(source, object.root_index);
        append_field(source, object.package_id);
        append_field(source, object.node_id);
        append_field(source, object.object_type);
        append_field(source, object.source_name);
        append_field(source, object.destination_name);
        append_field(source, object.normalized_sha256);
        append_integer(source, object.partition_index);
        append_field(source, object.group_name);
        append_field(source, object.volume_name);
        append_field(source, object.raw_group);
        append_field(source, object.raw_volume);
        for (const auto action : object.actions)
            append_field(source, package_import_action_name(action));
        append_field(source, object.canonical_action_id.value_or(""));
        append_field(source, object.existing_object_key.value_or(""));
        append_integer(source, object.target_sfs_id.value_or(0U));
        append_integer(source, object.target_link_id.value_or(0U));
        for (const auto number : object.target_program_numbers)
            append_integer(source, number);
        append_integer(source, object.target_grouped);
        append_integer(source, object.payload_clusters);
        append_integer(source, object.payload_sectors);
        append_integer(source, object.continuation_clusters);
    }
    for (const auto &delta : plan.allocation) {
        append_integer(source, delta.partition_index);
        append_field(source, delta.group_name);
        append_field(source, delta.volume_name);
        append_field(source, delta.raw_group);
        append_field(source, delta.raw_volume);
        append_integer(source, delta.inserted_object_count);
        append_integer(source, delta.reused_object_count);
        append_integer(source, delta.payload_clusters);
        append_integer(source, delta.payload_sectors);
        append_integer(source, delta.continuation_clusters);
        append_integer(source, delta.directory_growth_bytes);
        append_integer(source, delta.remaining_object_ids);
        append_integer(source, delta.remaining_clusters);
        append_integer(source, delta.projected_image_sectors);
        append_integer(source, delta.projected_image_size_bytes);
    }
    for (const auto &warning : plan.warnings) {
        append_field(source, warning.code);
        append_field(source, warning.message);
        append_integer(source, warning.fatal);
    }
    for (const auto &conflict : plan.conflicts) {
        append_field(source, conflict.code);
        append_field(source, conflict.message);
        append_field(source, conflict.package_id);
        append_field(source, conflict.node_id);
        append_integer(source, conflict.package_index.value_or(0U));
        append_integer(source, conflict.root_index.value_or(0U));
        append_integer(source, conflict.partition_index.value_or(0U));
        append_field(source, conflict.group_name);
        append_field(source, conflict.volume_name);
        append_field(source, conflict.raw_group);
        append_field(source, conflict.raw_volume);
    }
    return digest_text(source);
}

Result<PackageImportPlan> plan_fat12_import(const std::filesystem::path &target_path,
                                            std::span<const PortablePackage> packages,
                                            const PackageImportRequest &request, const MediaContainer &target,
                                            PackageImportPlan plan, const package_internal::Sha256Digest &before,
                                            const CancellationToken &cancellation) {
    const auto &fat = std::get<FatImage>(target.storage());
    auto catalog = build_object_catalog(target, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    for (const auto &issue : catalog->issues)
        add_conflict(plan, issue.code, issue.message);
    for (const auto &file : fat.files()) {
        if (file.path.find('/') != std::string::npos) {
            add_conflict(plan, "FAT12_PROFILE_UNSUPPORTED",
                         "package import supports only the Yamaha root-level "
                         "FAT12 profile");
            break;
        }
    }

    std::map<std::pair<std::size_t, std::size_t>, const PackageRootDestination *> destinations;
    for (const auto &destination : request.root_destinations) {
        if (destination.package_index >= packages.size() ||
            destination.root_index >=
                packages[std::min(destination.package_index, packages.size() - 1U)].roots.size()) {
            add_conflict(plan, "DESTINATION_ROOT_INVALID", "root destination references a missing package or root",
                         &destination);
            continue;
        }
        if (!destination.partition_index || *destination.partition_index != 0U || !destination.group_name.empty() ||
            destination.volume_name != "FAT root" || !destination.raw_group.empty() ||
            !destination.raw_volume.empty() || destination.create_destination) {
            add_conflict(plan, "FAT12_DESTINATION_INVALID",
                         "Yamaha FAT12 imports require partition 0 and the "
                         "existing FAT root",
                         &destination, &packages[destination.package_index]);
            continue;
        }
        const auto key = std::pair{destination.package_index, destination.root_index};
        if (!destinations.emplace(key, &destination).second) {
            add_conflict(plan, "DESTINATION_ROOT_DUPLICATE", "package root has more than one destination mapping",
                         &destination, &packages[destination.package_index]);
        }
    }
    for (std::size_t package_index = 0; package_index < packages.size(); ++package_index) {
        for (std::size_t root_index = 0; root_index < packages[package_index].roots.size(); ++root_index) {
            if (!destinations.contains({package_index, root_index})) {
                PackageRootDestination missing;
                missing.package_index = package_index;
                missing.root_index = root_index;
                add_conflict(plan, "DESTINATION_ROOT_MISSING", "every package root requires one explicit destination",
                             &missing, &packages[package_index]);
            }
        }
    }

    std::map<std::pair<std::size_t, std::string>, std::string> renames;
    for (const auto &rename : request.policy.renames) {
        if (rename.package_index >= packages.size() ||
            node_by_id(packages[rename.package_index], rename.node_id) == nullptr ||
            !valid_sfs_name(rename.destination_name) ||
            !renames.emplace(std::pair{rename.package_index, rename.node_id}, rename.destination_name).second) {
            add_conflict(plan, "PACKAGE_RENAME_INVALID",
                         "package rename must identify one node and contain 1 "
                         "to 16 ASCII bytes");
        }
    }

    std::vector<Candidate> candidates;
    for (const auto &[key, destination] : destinations) {
        const auto &[package_index, root_index] = key;
        const auto &package = packages[package_index];
        const auto closure = root_closure(package, root_index);
        std::map<std::string, std::string, std::less<>> names;
        for (const auto *node : closure) {
            auto name = node->name;
            if (const auto renamed = renames.find({package_index, node->node_id}); renamed != renames.end()) {
                name = renamed->second;
            }
            names.emplace(node->node_id, std::move(name));
        }
        for (const auto *node : closure) {
            const auto &name = names.at(node->node_id);
            if (!valid_sfs_name(name)) {
                add_conflict(plan, "FAT12_OBJECT_NAME_INVALID", "Yamaha object names must contain 1 to 16 ASCII bytes",
                             destination, &package, node);
                continue;
            }
            if (node->object_type == "PROG") {
                PlannedPackageObject program;
                program.destination_name = name;
                if (const auto number = planned_program_number(program); !number) {
                    add_conflict(plan, "FAT12_PROGRAM_SLOT_INVALID", number.error().message, destination, &package,
                                 node);
                    continue;
                }
            }
            auto normalized = projected_normalized_sha256(package, *node, names);
            if (!normalized)
                return std::unexpected{normalized.error()};
            candidates.push_back({&package, node, destination, name, std::move(*normalized)});
        }
    }

    PlannedPackageDestination fat_destination;
    fat_destination.volume_name = "FAT root";
    plan.destinations.push_back(std::move(fat_destination));
    std::ranges::sort(candidates, [](const Candidate &left, const Candidate &right) {
        return std::tuple{type_rank(left.node->object_type),
                          left.destination_name,
                          left.projected_normalized_sha256,
                          left.package->package_id,
                          left.destination->package_index,
                          left.destination->root_index,
                          left.node->node_id} < std::tuple{type_rank(right.node->object_type),
                                                           right.destination_name,
                                                           right.projected_normalized_sha256,
                                                           right.package->package_id,
                                                           right.destination->package_index,
                                                           right.destination->root_index,
                                                           right.node->node_id};
    });

    const auto existing = existing_objects(*catalog);
    std::set<std::uint32_t> used_link_ids;
    for (const auto &item : existing) {
        if (item.link_id)
            used_link_ids.insert(*item.link_id);
    }
    std::uint32_t next_link_id = 0x016b1dbcU;
    std::map<std::pair<std::string, std::string>, std::size_t> planned_names;
    for (const auto &candidate : candidates) {
        PlannedPackageObject object;
        object.action_id = action_identity(candidate);
        object.package_index = candidate.destination->package_index;
        object.root_index = candidate.destination->root_index;
        object.package_id = candidate.package->package_id;
        object.node_id = candidate.node->node_id;
        object.object_type = candidate.node->object_type;
        object.source_name = candidate.node->name;
        object.destination_name = candidate.destination_name;
        object.normalized_sha256 = candidate.projected_normalized_sha256;
        object.partition_index = 0U;
        object.volume_name = "FAT root";
        if (object.source_name != object.destination_name)
            object.actions.push_back(PackageImportObjectAction::rename);

        std::vector<const ExistingObject *> matches;
        for (const auto &item : existing) {
            if (item.snapshot->object.header.raw_type == object.object_type &&
                item.snapshot->object.header.name == object.destination_name) {
                matches.push_back(&item);
            }
        }
        if (matches.size() > 1U) {
            mark_conflict(object);
            add_conflict(plan, "FAT12_TARGET_NAME_AMBIGUOUS",
                         "FAT root contains multiple objects with the same "
                         "type and name",
                         candidate.destination, candidate.package, candidate.node);
        } else if (matches.size() == 1U) {
            if (matches.front()->normalized_sha256 == object.normalized_sha256) {
                object.actions.push_back(PackageImportObjectAction::reuse);
                object.existing_object_key = matches.front()->snapshot->key;
                object.target_link_id = matches.front()->link_id;
            } else {
                mark_conflict(object);
                add_conflict(plan, "FAT12_NAME_CONFLICT",
                             "FAT root already contains the same object name "
                             "with different content",
                             candidate.destination, candidate.package, candidate.node);
            }
        } else {
            const auto name_key = std::pair{object.object_type, object.destination_name};
            if (const auto found = planned_names.find(name_key); found != planned_names.end()) {
                const auto &canonical = plan.objects[found->second];
                if (canonical.normalized_sha256 == object.normalized_sha256 &&
                    !std::ranges::contains(canonical.actions, PackageImportObjectAction::conflict)) {
                    object.actions.push_back(PackageImportObjectAction::reuse);
                    object.canonical_action_id = canonical.action_id;
                    object.target_link_id = canonical.target_link_id;
                } else {
                    mark_conflict(object);
                    mark_conflict(plan.objects[found->second]);
                    add_conflict(plan, "FAT12_NAME_CONFLICT",
                                 "incoming roots assign different content to "
                                 "the same FAT object name",
                                 candidate.destination, candidate.package, candidate.node);
                }
            } else {
                if (!candidate.node->relocations.empty())
                    object.actions.push_back(PackageImportObjectAction::relocate);
                object.actions.push_back(PackageImportObjectAction::insert);
                if (object.object_type == "SMPL") {
                    while (used_link_ids.contains(next_link_id))
                        next_link_id += 0x100U;
                    object.target_link_id = next_link_id;
                    used_link_ids.insert(next_link_id);
                    next_link_id += 0x100U;
                }
                planned_names.emplace(name_key, plan.objects.size());
            }
        }
        plan.objects.push_back(std::move(object));
    }

    std::map<std::string, const PlannedPackageObject *, std::less<>> actions_by_id;
    for (const auto &object : plan.objects)
        actions_by_id.emplace(object.action_id, &object);
    for (auto &object : plan.objects) {
        if (!object.canonical_action_id)
            continue;
        const auto canonical = actions_by_id.find(*object.canonical_action_id);
        if (canonical == actions_by_id.end()) {
            mark_conflict(object);
            add_conflict(plan, "PLANNED_CANONICAL_OBJECT_MISSING",
                         "reused incoming object has no canonical planned allocation");
            continue;
        }
        object.target_link_id = canonical->second->target_link_id;
        if (std::ranges::contains(canonical->second->actions, PackageImportObjectAction::conflict))
            mark_conflict(object);
    }

    struct BankMetadata {
        std::set<std::uint8_t> programs;
        bool grouped{};
    };
    const auto physical_key = [](const PlannedPackageObject &object) {
        if (object.existing_object_key)
            return "existing:" + *object.existing_object_key;
        if (object.canonical_action_id)
            return "planned:" + *object.canonical_action_id;
        return "planned:" + object.action_id;
    };
    std::map<std::string, BankMetadata, std::less<>> bank_metadata;
    for (const auto &object : plan.objects) {
        if (object.object_type != "SBNK")
            continue;
        auto &metadata = bank_metadata[physical_key(object)];
        if (!object.existing_object_key)
            continue;
        const auto found = std::ranges::find_if(
            existing, [&](const auto &candidate) { return candidate.snapshot->key == *object.existing_object_key; });
        if (found != existing.end()) {
            if (const auto *bank = std::get_if<CurrentSbnk>(&found->snapshot->object.payload)) {
                metadata.programs.insert(bank->linked_program_numbers.begin(), bank->linked_program_numbers.end());
                metadata.grouped = (bank->sample_flags & 1U) != 0U;
            }
        }
    }
    for (const auto &owner : plan.objects) {
        if (std::ranges::contains(owner.actions, PackageImportObjectAction::conflict))
            continue;
        const auto &package = packages[owner.package_index];
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != owner.node_id ||
                (edge.role != "SBAC_SLOT_TO_SBNK" && edge.role != "PROG_ASSIGNMENT_TO_SBNK")) {
                continue;
            }
            const auto *target_action = planned_node(plan, owner, edge.target_node_id);
            if (target_action == nullptr || target_action->object_type != "SBNK")
                continue;
            auto &metadata = bank_metadata[physical_key(*target_action)];
            if (edge.role == "SBAC_SLOT_TO_SBNK") {
                metadata.grouped = true;
            } else {
                const auto number = planned_program_number(owner);
                if (!number)
                    return std::unexpected{number.error()};
                metadata.programs.insert(*number);
            }
        }
    }
    for (auto &object : plan.objects) {
        if (object.object_type != "SBNK")
            continue;
        const auto metadata = bank_metadata.find(physical_key(object));
        if (metadata == bank_metadata.end())
            continue;
        object.target_program_numbers.assign(metadata->second.programs.begin(), metadata->second.programs.end());
        object.target_grouped = metadata->second.grouped;
    }

    if (plan.conflicts.empty()) {
        for (auto &object : plan.objects) {
            const auto *node = node_by_id(packages[object.package_index], object.node_id);
            if (node == nullptr)
                return std::unexpected{planner_error("planned FAT12 package node is missing")};
            if (object.object_type == "SMPL" && object.existing_object_key && !object.target_link_id) {
                continue;
            }
            auto context = relocation_context(packages[object.package_index], plan, object);
            if (!context)
                return std::unexpected{context.error()};
            auto relocated = package_internal::relocate_package_node(packages[object.package_index], *node, *context);
            if (!relocated)
                return std::unexpected{relocated.error()};
            auto decoded = decode_object(*relocated);
            if (!decoded)
                return std::unexpected{decoded.error()};
            auto profile = package_internal::build_relocation_profile(*decoded, *relocated);
            if (!profile)
                return std::unexpected{profile.error()};
            const auto normalized = package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
            if (normalized != object.normalized_sha256)
                return std::unexpected{planner_error("planned FAT12 relocation changed object identity")};
            if (std::ranges::contains(object.actions, PackageImportObjectAction::insert)) {
                if (*relocated != node->raw_payload &&
                    !std::ranges::contains(object.actions, PackageImportObjectAction::relocate)) {
                    object.actions.push_back(PackageImportObjectAction::relocate);
                }
                continue;
            }
            if (!object.existing_object_key)
                continue;
            const auto found = std::ranges::find_if(existing, [&](const auto &candidate) {
                return candidate.snapshot->key == *object.existing_object_key;
            });
            if (found == existing.end())
                return std::unexpected{planner_error("planned FAT12 existing object is missing")};
            if (*relocated != found->snapshot->raw_payload) {
                if (object.object_type != "SBNK") {
                    return std::unexpected{planner_error("existing FAT12 object relocation fields "
                                                         "differ from the target")};
                }
                object.actions.push_back(PackageImportObjectAction::relocate);
            }
        }
    }

    std::set<std::string, std::less<>> media_object_paths;
    const auto media_objects = target.objects(64U * 1024U * 1024U, cancellation);
    if (!media_objects)
        return std::unexpected{media_objects.error()};
    for (const auto &object : *media_objects)
        media_object_paths.insert(object.logical_path);
    std::size_t retained_files{};
    for (const auto &file : fat.files()) {
        if (!media_object_paths.contains(file.path))
            ++retained_files;
    }
    std::uint64_t inserted_objects{};
    std::uint64_t reused_objects{};
    for (const auto &object : plan.objects) {
        if (std::ranges::contains(object.actions, PackageImportObjectAction::insert) &&
            !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            ++inserted_objects;
        } else if (std::ranges::contains(object.actions, PackageImportObjectAction::reuse) &&
                   !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            ++reused_objects;
        }
    }
    const auto final_entries = catalog->objects.size() + retained_files + inserted_objects;
    if (final_entries > fat.geometry().root_entry_count)
        add_conflict(plan, "FAT12_ROOT_ENTRY_EXHAUSTED", "FAT12 root directory cannot contain the planned import");
    const auto cluster_size = fat.geometry().cluster_size();
    std::uint64_t used_clusters{};
    for (const auto &file : fat.files())
        used_clusters += (file.size + cluster_size - 1U) / cluster_size;
    for (auto &object : plan.objects) {
        if (!std::ranges::contains(object.actions, PackageImportObjectAction::insert) ||
            std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            continue;
        }
        const auto *node = node_by_id(packages[object.package_index], object.node_id);
        const auto clusters = (node->raw_payload.size() + cluster_size - 1U) / cluster_size;
        used_clusters += clusters;
        object.payload_clusters = clusters;
    }
    if (used_clusters > fat.geometry().data_cluster_count)
        add_conflict(plan, "FAT12_CLUSTER_EXHAUSTED", "FAT12 data area cannot contain the planned import");

    PackageAllocationDelta delta;
    delta.volume_name = "FAT root";
    delta.inserted_object_count = inserted_objects;
    delta.reused_object_count = reused_objects;
    for (const auto &object : plan.objects)
        delta.payload_clusters += object.payload_clusters;
    delta.directory_growth_bytes = inserted_objects * 32U;
    delta.remaining_object_ids =
        final_entries > fat.geometry().root_entry_count ? 0U : fat.geometry().root_entry_count - final_entries;
    delta.remaining_clusters =
        used_clusters > fat.geometry().data_cluster_count ? 0U : fat.geometry().data_cluster_count - used_clusters;
    plan.allocation.push_back(std::move(delta));

    auto final_reader = FileReader::open(target_path);
    if (!final_reader)
        return std::unexpected{final_reader.error()};
    const auto after = package_internal::sha256_reader(**final_reader, cancellation);
    if (!after)
        return std::unexpected{after.error()};
    if (*after != before)
        return std::unexpected{planner_error("target image changed while its import plan was built")};
    std::ranges::sort(plan.conflicts, [](const auto &left, const auto &right) {
        return std::tie(left.code, left.package_index, left.root_index, left.package_id, left.node_id,
                        left.partition_index, left.group_name, left.volume_name, left.raw_group, left.raw_volume,
                        left.message) < std::tie(right.code, right.package_index, right.root_index, right.package_id,
                                                 right.node_id, right.partition_index, right.group_name,
                                                 right.volume_name, right.raw_group, right.raw_volume, right.message);
    });
    plan.plan_id = plan_identity(plan);
    return plan;
}

Result<PackageImportPlan> plan_iso9660_import(const std::filesystem::path &target_path,
                                              std::span<const PortablePackage> packages,
                                              const PackageImportRequest &request, const MediaContainer &target,
                                              PackageImportPlan plan, const package_internal::Sha256Digest &before,
                                              const CancellationToken &cancellation) {
    const auto &iso = std::get<IsoImage>(target.storage());
    for (const auto &issue : iso.validation_issues())
        add_conflict(plan, issue.code, issue.message);

    struct IsoScope {
        std::string group_name;
        std::string volume_name;
    };
    using IsoScopeKey = std::pair<std::string, std::string>;
    std::map<std::string, std::string, std::less<>> group_labels;
    for (const auto &[raw, label] : iso.group_labels())
        group_labels.emplace(raw, label);
    std::map<IsoScopeKey, std::string> volume_labels;
    for (const auto &[raw_path, label] : iso.volume_labels()) {
        const auto separator = raw_path.find('/');
        if (separator != std::string::npos)
            volume_labels.emplace(IsoScopeKey{raw_path.substr(0, separator), raw_path.substr(separator + 1U)}, label);
    }
    std::map<IsoScopeKey, IsoScope> scopes;
    for (const auto &file : iso.files()) {
        if (!file.is_directory)
            continue;
        const auto separator = file.path.find('/');
        if (separator == std::string::npos || file.path.find('/', separator + 1U) != std::string::npos)
            continue;
        const auto key = IsoScopeKey{file.path.substr(0, separator), file.path.substr(separator + 1U)};
        if (!valid_iso_raw_group(key.first) || !valid_iso_raw_volume(key.second))
            continue;
        const auto group = group_labels.find(key.first);
        const auto volume = volume_labels.find(key);
        if (group == group_labels.end() || volume == volume_labels.end()) {
            add_conflict(plan, "ISO9660_LABEL_METADATA_MISSING",
                         "an existing Yamaha raw volume lacks confirmed group "
                         "or volume catalog labels");
            continue;
        }
        scopes.emplace(key, IsoScope{group->second, volume->second});
    }
    if (scopes.empty()) {
        add_conflict(plan, "ISO9660_PROFILE_UNSUPPORTED",
                     "package import requires at least one cataloged Yamaha ISO volume");
    }
    std::map<std::string, std::vector<std::string>, std::less<>> existing_group_volumes;
    for (const auto &[scope, ignored] : scopes) {
        (void)ignored;
        existing_group_volumes[scope.first].push_back(scope.second);
    }
    for (auto &[group, volumes] : existing_group_volumes) {
        std::ranges::sort(volumes);
        for (std::size_t index = 0; index < volumes.size(); ++index) {
            if (volumes[index] != std::format("F{:03}", index + 1U)) {
                add_conflict(plan, "ISO9660_RAW_VOLUME_SEQUENCE_INVALID",
                             std::format("raw group '{}' does not use "
                                         "contiguous volumes F001..Fnnn",
                                         group));
                break;
            }
        }
    }

    std::vector<PackageRootDestination> normalized_destinations;
    normalized_destinations.reserve(request.root_destinations.size());
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> destination_indices;
    std::map<IsoScopeKey, IsoScope> planned_scopes = scopes;
    std::map<IsoScopeKey, bool> destination_creation;
    for (const auto &requested : request.root_destinations) {
        auto destination = requested;
        if (destination.package_index >= packages.size() ||
            destination.root_index >=
                packages[std::min(destination.package_index, packages.size() - 1U)].roots.size()) {
            add_conflict(plan, "DESTINATION_ROOT_INVALID", "root destination references a missing package or root",
                         &destination);
            continue;
        }
        if (!destination.partition_index || *destination.partition_index != 0U ||
            !valid_iso_raw_group(destination.raw_group) || destination.group_name.empty() ||
            destination.group_name.size() > 16U || destination.volume_name.empty() ||
            destination.volume_name.size() > 16U) {
            add_conflict(plan, "ISO9660_DESTINATION_INVALID",
                         "ISO destinations require partition 0, a raw group, "
                         "and bounded sampler "
                         "group and volume labels",
                         &destination, &packages[destination.package_index]);
            continue;
        }

        std::size_t group_volume_count{};
        for (const auto &[scope, ignored] : planned_scopes) {
            (void)ignored;
            if (scope.first == destination.raw_group)
                ++group_volume_count;
        }
        if (destination.create_destination) {
            const auto next_raw = std::format("F{:03}", group_volume_count + 1U);
            const auto requested_raw = destination.raw_volume;
            if (destination.raw_volume.empty())
                destination.raw_volume = next_raw;
            if (!valid_iso_raw_volume(destination.raw_volume) || group_volume_count >= 999U) {
                add_conflict(plan, "ISO9660_RAW_VOLUME_ALLOCATION_INVALID",
                             "new ISO destinations must use the next "
                             "contiguous Fnnn raw volume",
                             &destination, &packages[destination.package_index]);
                continue;
            }
            if (const auto group = group_labels.find(destination.raw_group);
                group != group_labels.end() && group->second != destination.group_name) {
                add_conflict(plan, "ISO9660_GROUP_LABEL_CONFLICT",
                             "new ISO volume uses a different label for an "
                             "existing raw group",
                             &destination, &packages[destination.package_index]);
                continue;
            }
            const IsoScopeKey scope{destination.raw_group, destination.raw_volume};
            if (const auto existing = scopes.find(scope); existing != scopes.end()) {
                add_conflict(plan, "ISO9660_DESTINATION_ALREADY_EXISTS",
                             "ISO destination creation requested an existing raw volume", &destination,
                             &packages[destination.package_index]);
                continue;
            }
            if (const auto planned = planned_scopes.find(scope); planned != planned_scopes.end()) {
                if (planned->second.group_name != destination.group_name ||
                    planned->second.volume_name != destination.volume_name || !destination_creation.contains(scope) ||
                    !destination_creation.at(scope)) {
                    add_conflict(plan, "ISO9660_DESTINATION_POLICY_CONFLICT",
                                 "package roots disagree about one newly "
                                 "created raw volume",
                                 &destination, &packages[destination.package_index]);
                    continue;
                }
            } else {
                if (!requested_raw.empty() && requested_raw != next_raw) {
                    add_conflict(plan, "ISO9660_RAW_VOLUME_ALLOCATION_INVALID",
                                 "new ISO destinations must use the next "
                                 "contiguous Fnnn raw volume",
                                 &destination, &packages[destination.package_index]);
                    continue;
                }
                planned_scopes.emplace(scope, IsoScope{destination.group_name, destination.volume_name});
            }
        } else {
            if (!valid_iso_raw_volume(destination.raw_volume)) {
                add_conflict(plan, "ISO9660_DESTINATION_INVALID",
                             "existing ISO destinations require an explicit "
                             "F001..F999 raw volume",
                             &destination, &packages[destination.package_index]);
                continue;
            }
            const auto found = scopes.find({destination.raw_group, destination.raw_volume});
            if (found == scopes.end()) {
                add_conflict(plan, "ISO9660_DESTINATION_MISSING", "ISO destination raw volume does not exist",
                             &destination, &packages[destination.package_index]);
                continue;
            }
            if (found->second.group_name != destination.group_name ||
                found->second.volume_name != destination.volume_name) {
                add_conflict(plan, "ISO9660_DESTINATION_LABEL_MISMATCH",
                             "ISO destination labels do not match the existing "
                             "Yamaha catalogs",
                             &destination, &packages[destination.package_index]);
                continue;
            }
        }
        const IsoScopeKey scope{destination.raw_group, destination.raw_volume};
        const auto [policy, policy_inserted] = destination_creation.emplace(scope, destination.create_destination);
        if (!policy_inserted && policy->second != destination.create_destination) {
            add_conflict(plan, "ISO9660_DESTINATION_POLICY_CONFLICT",
                         "package roots disagree about raw-volume creation", &destination,
                         &packages[destination.package_index]);
            continue;
        }
        const auto root_key = std::pair{destination.package_index, destination.root_index};
        if (destination_indices.contains(root_key)) {
            add_conflict(plan, "DESTINATION_ROOT_DUPLICATE", "package root has more than one destination mapping",
                         &destination, &packages[destination.package_index]);
            continue;
        }
        destination_indices.emplace(root_key, normalized_destinations.size());
        normalized_destinations.push_back(std::move(destination));
    }
    for (std::size_t package_index = 0; package_index < packages.size(); ++package_index) {
        for (std::size_t root_index = 0; root_index < packages[package_index].roots.size(); ++root_index) {
            if (!destination_indices.contains({package_index, root_index})) {
                PackageRootDestination missing;
                missing.package_index = package_index;
                missing.root_index = root_index;
                add_conflict(plan, "DESTINATION_ROOT_MISSING", "every package root requires one explicit destination",
                             &missing, &packages[package_index]);
            }
        }
    }

    std::map<std::pair<std::size_t, std::string>, std::string> renames;
    for (const auto &rename : request.policy.renames) {
        if (rename.package_index >= packages.size() ||
            node_by_id(packages[rename.package_index], rename.node_id) == nullptr ||
            !valid_sfs_name(rename.destination_name) ||
            !renames.emplace(std::pair{rename.package_index, rename.node_id}, rename.destination_name).second) {
            add_conflict(plan, "PACKAGE_RENAME_INVALID",
                         "package rename must identify one node and contain 1 "
                         "to 16 ASCII bytes");
        }
    }

    std::vector<Candidate> candidates;
    for (const auto &[key, destination_index] : destination_indices) {
        const auto &[package_index, root_index] = key;
        const auto &package = packages[package_index];
        const auto *destination = &normalized_destinations[destination_index];
        const auto closure = root_closure(package, root_index);
        std::map<std::string, std::string, std::less<>> names;
        for (const auto *node : closure) {
            auto name = node->name;
            if (const auto renamed = renames.find({package_index, node->node_id}); renamed != renames.end())
                name = renamed->second;
            names.emplace(node->node_id, std::move(name));
        }
        for (const auto *node : closure) {
            const auto &name = names.at(node->node_id);
            if (!valid_sfs_name(name)) {
                add_conflict(plan, "ISO9660_OBJECT_NAME_INVALID",
                             "Yamaha object names must contain 1 to 16 ASCII bytes", destination, &package, node);
                continue;
            }
            if (node->object_type == "PROG") {
                PlannedPackageObject program;
                program.destination_name = name;
                if (const auto number = planned_program_number(program); !number) {
                    add_conflict(plan, "ISO9660_PROGRAM_SLOT_INVALID", number.error().message, destination, &package,
                                 node);
                    continue;
                }
            }
            auto normalized = projected_normalized_sha256(package, *node, names);
            if (!normalized)
                return std::unexpected{normalized.error()};
            candidates.push_back({&package, node, destination, name, std::move(*normalized)});
        }
    }

    for (const auto &[scope, create] : destination_creation) {
        const auto &labels = planned_scopes.at(scope);
        PlannedPackageDestination destination;
        destination.group_name = labels.group_name;
        destination.volume_name = labels.volume_name;
        destination.raw_group = scope.first;
        destination.raw_volume = scope.second;
        destination.create = create;
        plan.destinations.push_back(std::move(destination));
    }
    std::ranges::sort(plan.destinations, [](const auto &left, const auto &right) {
        return std::tie(left.raw_group, left.raw_volume) < std::tie(right.raw_group, right.raw_volume);
    });
    std::ranges::sort(candidates, [](const Candidate &left, const Candidate &right) {
        return std::tuple{
                   left.destination->raw_group,     left.destination->raw_volume,     type_rank(left.node->object_type),
                   left.destination_name,           left.projected_normalized_sha256, left.package->package_id,
                   left.destination->package_index, left.destination->root_index,     left.node->node_id} <
               std::tuple{right.destination->raw_group,
                          right.destination->raw_volume,
                          type_rank(right.node->object_type),
                          right.destination_name,
                          right.projected_normalized_sha256,
                          right.package->package_id,
                          right.destination->package_index,
                          right.destination->root_index,
                          right.node->node_id};
    });

    auto catalog = build_object_catalog(target, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    for (const auto &issue : catalog->issues)
        add_conflict(plan, issue.code, issue.message);
    const auto existing = existing_objects(*catalog);
    std::map<IsoScopeKey, std::set<std::uint32_t>> used_link_ids;
    for (const auto &item : existing) {
        const auto scope = iso_raw_scope(*item.snapshot);
        if (scope && item.link_id)
            used_link_ids[*scope].insert(*item.link_id);
    }
    std::map<IsoScopeKey, std::uint32_t> next_link_ids;
    for (const auto &[scope, ignored] : planned_scopes) {
        (void)ignored;
        next_link_ids.emplace(scope, 0x016b1dbcU);
    }
    std::map<std::tuple<std::string, std::string, std::string, std::string>, std::size_t> planned_names;
    std::map<std::tuple<std::string, std::string, std::string>, std::set<IsoScopeKey>> planned_equal_scopes;
    std::set<std::tuple<std::string, std::string, std::string>> duplicate_warnings;
    for (const auto &candidate : candidates) {
        PlannedPackageObject object;
        object.action_id = action_identity(candidate);
        object.package_index = candidate.destination->package_index;
        object.root_index = candidate.destination->root_index;
        object.package_id = candidate.package->package_id;
        object.node_id = candidate.node->node_id;
        object.object_type = candidate.node->object_type;
        object.source_name = candidate.node->name;
        object.destination_name = candidate.destination_name;
        object.normalized_sha256 = candidate.projected_normalized_sha256;
        object.group_name = candidate.destination->group_name;
        object.volume_name = candidate.destination->volume_name;
        object.raw_group = candidate.destination->raw_group;
        object.raw_volume = candidate.destination->raw_volume;
        if (object.source_name != object.destination_name)
            object.actions.push_back(PackageImportObjectAction::rename);

        std::vector<const ExistingObject *> matches;
        bool equal_elsewhere{};
        for (const auto &item : existing) {
            const auto scope = iso_raw_scope(*item.snapshot);
            if (!scope || item.snapshot->object.header.raw_type != object.object_type ||
                item.snapshot->object.header.name != object.destination_name)
                continue;
            if (*scope == IsoScopeKey{object.raw_group, object.raw_volume})
                matches.push_back(&item);
            else if (item.normalized_sha256 == object.normalized_sha256)
                equal_elsewhere = true;
        }
        if (matches.size() > 1U) {
            mark_conflict(object);
            add_conflict(plan, "ISO9660_TARGET_NAME_AMBIGUOUS",
                         "raw volume contains multiple objects with the same "
                         "type and name",
                         candidate.destination, candidate.package, candidate.node);
        } else if (matches.size() == 1U) {
            if (matches.front()->normalized_sha256 == object.normalized_sha256) {
                object.actions.push_back(PackageImportObjectAction::reuse);
                object.existing_object_key = matches.front()->snapshot->key;
                object.target_link_id = matches.front()->link_id;
            } else {
                mark_conflict(object);
                add_conflict(plan, "ISO9660_NAME_CONFLICT",
                             "raw volume already contains the same object name "
                             "with different content",
                             candidate.destination, candidate.package, candidate.node);
            }
        } else {
            const auto name_key =
                std::tuple{object.raw_group, object.raw_volume, object.object_type, object.destination_name};
            if (const auto found = planned_names.find(name_key); found != planned_names.end()) {
                const auto &canonical = plan.objects[found->second];
                if (canonical.normalized_sha256 == object.normalized_sha256 &&
                    !std::ranges::contains(canonical.actions, PackageImportObjectAction::conflict)) {
                    object.actions.push_back(PackageImportObjectAction::reuse);
                    object.canonical_action_id = canonical.action_id;
                    object.target_link_id = canonical.target_link_id;
                } else {
                    mark_conflict(object);
                    mark_conflict(plan.objects[found->second]);
                    add_conflict(plan, "ISO9660_NAME_CONFLICT",
                                 "incoming roots assign different content to "
                                 "the same raw-volume object name",
                                 candidate.destination, candidate.package, candidate.node);
                }
            } else {
                if (!candidate.node->relocations.empty())
                    object.actions.push_back(PackageImportObjectAction::relocate);
                object.actions.push_back(PackageImportObjectAction::insert);
                if (object.object_type == "SMPL") {
                    const IsoScopeKey scope{object.raw_group, object.raw_volume};
                    auto &next = next_link_ids.at(scope);
                    while (used_link_ids[scope].contains(next))
                        next += 0x100U;
                    object.target_link_id = next;
                    used_link_ids[scope].insert(next);
                    next += 0x100U;
                }
                planned_names.emplace(name_key, plan.objects.size());
                const auto identity_key =
                    std::tuple{object.object_type, object.destination_name, object.normalized_sha256};
                const auto &equal_scopes = planned_equal_scopes[identity_key];
                const auto equal_planned_elsewhere = std::ranges::any_of(equal_scopes, [&](const auto &scope) {
                    return scope != IsoScopeKey{object.raw_group, object.raw_volume};
                });
                if ((equal_elsewhere || equal_planned_elsewhere) &&
                    duplicate_warnings.emplace(object.raw_group, object.raw_volume, object.node_id).second) {
                    plan.warnings.push_back(
                        {"ISO9660_CROSS_VOLUME_DUPLICATE",
                         std::format("{} '{}' is duplicated in raw volume "
                                     "{}/{} because ISO reuse is "
                                     "volume-local",
                                     object.object_type, object.destination_name, object.raw_group, object.raw_volume),
                         false});
                }
                planned_equal_scopes[identity_key].emplace(object.raw_group, object.raw_volume);
            }
        }
        plan.objects.push_back(std::move(object));
    }

    std::map<std::string, const PlannedPackageObject *, std::less<>> actions_by_id;
    for (const auto &object : plan.objects)
        actions_by_id.emplace(object.action_id, &object);
    for (auto &object : plan.objects) {
        if (!object.canonical_action_id)
            continue;
        const auto canonical = actions_by_id.find(*object.canonical_action_id);
        if (canonical == actions_by_id.end()) {
            mark_conflict(object);
            add_conflict(plan, "PLANNED_CANONICAL_OBJECT_MISSING",
                         "reused incoming object has no canonical planned allocation");
            continue;
        }
        object.target_link_id = canonical->second->target_link_id;
        if (std::ranges::contains(canonical->second->actions, PackageImportObjectAction::conflict))
            mark_conflict(object);
    }

    struct BankMetadata {
        std::set<std::uint8_t> programs;
        bool grouped{};
    };
    const auto physical_key = [](const PlannedPackageObject &object) {
        const auto prefix = object.raw_group + "/" + object.raw_volume + ":";
        if (object.existing_object_key)
            return prefix + "existing:" + *object.existing_object_key;
        if (object.canonical_action_id)
            return prefix + "planned:" + *object.canonical_action_id;
        return prefix + "planned:" + object.action_id;
    };
    std::map<std::string, BankMetadata, std::less<>> bank_metadata;
    for (const auto &object : plan.objects) {
        if (object.object_type != "SBNK")
            continue;
        auto &metadata = bank_metadata[physical_key(object)];
        if (!object.existing_object_key)
            continue;
        const auto found = std::ranges::find_if(
            existing, [&](const auto &candidate) { return candidate.snapshot->key == *object.existing_object_key; });
        if (found != existing.end()) {
            if (const auto *bank = std::get_if<CurrentSbnk>(&found->snapshot->object.payload)) {
                metadata.programs.insert(bank->linked_program_numbers.begin(), bank->linked_program_numbers.end());
                metadata.grouped = (bank->sample_flags & 1U) != 0U;
            }
        }
    }
    for (const auto &owner : plan.objects) {
        if (std::ranges::contains(owner.actions, PackageImportObjectAction::conflict))
            continue;
        const auto &package = packages[owner.package_index];
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != owner.node_id ||
                (edge.role != "SBAC_SLOT_TO_SBNK" && edge.role != "PROG_ASSIGNMENT_TO_SBNK"))
                continue;
            const auto *target_action = planned_node(plan, owner, edge.target_node_id);
            if (target_action == nullptr || target_action->object_type != "SBNK")
                continue;
            auto &metadata = bank_metadata[physical_key(*target_action)];
            if (edge.role == "SBAC_SLOT_TO_SBNK") {
                metadata.grouped = true;
            } else {
                const auto number = planned_program_number(owner);
                if (!number)
                    return std::unexpected{number.error()};
                metadata.programs.insert(*number);
            }
        }
    }
    for (auto &object : plan.objects) {
        if (object.object_type != "SBNK")
            continue;
        const auto metadata = bank_metadata.find(physical_key(object));
        if (metadata == bank_metadata.end())
            continue;
        object.target_program_numbers.assign(metadata->second.programs.begin(), metadata->second.programs.end());
        object.target_grouped = metadata->second.grouped;
    }

    if (plan.conflicts.empty()) {
        for (auto &object : plan.objects) {
            const auto *node = node_by_id(packages[object.package_index], object.node_id);
            if (node == nullptr)
                return std::unexpected{planner_error("planned ISO9660 package node is missing")};
            if (object.object_type == "SMPL" && object.existing_object_key && !object.target_link_id) {
                continue;
            }
            auto context = relocation_context(packages[object.package_index], plan, object);
            if (!context)
                return std::unexpected{context.error()};
            auto relocated = package_internal::relocate_package_node(packages[object.package_index], *node, *context);
            if (!relocated)
                return std::unexpected{relocated.error()};
            auto decoded = decode_object(*relocated);
            if (!decoded)
                return std::unexpected{decoded.error()};
            auto profile = package_internal::build_relocation_profile(*decoded, *relocated);
            if (!profile)
                return std::unexpected{profile.error()};
            const auto normalized = package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
            if (normalized != object.normalized_sha256)
                return std::unexpected{planner_error("planned ISO9660 relocation changed object identity")};
            if (std::ranges::contains(object.actions, PackageImportObjectAction::insert)) {
                if (*relocated != node->raw_payload &&
                    !std::ranges::contains(object.actions, PackageImportObjectAction::relocate))
                    object.actions.push_back(PackageImportObjectAction::relocate);
                continue;
            }
            if (!object.existing_object_key)
                continue;
            const auto found = std::ranges::find_if(existing, [&](const ExistingObject &candidate) {
                return candidate.snapshot->key == *object.existing_object_key;
            });
            if (found == existing.end())
                return std::unexpected{planner_error("planned ISO9660 existing object is missing")};
            if (*relocated != found->snapshot->raw_payload) {
                if (object.object_type != "SBNK") {
                    return std::unexpected{planner_error("existing ISO9660 object relocation "
                                                         "fields differ from the target")};
                }
                object.actions.push_back(PackageImportObjectAction::relocate);
            }
        }
    }

    std::map<std::tuple<std::string, std::string, std::string>, std::size_t> category_counts;
    for (const auto &item : existing) {
        if (const auto scope = iso_raw_scope(*item.snapshot); scope)
            ++category_counts[{scope->first, scope->second, item.snapshot->object.header.raw_type}];
    }
    std::map<IsoScopeKey, PackageAllocationDelta> allocation;
    for (const auto &destination : plan.destinations) {
        auto &delta = allocation[{destination.raw_group, destination.raw_volume}];
        delta.group_name = destination.group_name;
        delta.volume_name = destination.volume_name;
        delta.raw_group = destination.raw_group;
        delta.raw_volume = destination.raw_volume;
    }
    for (auto &object : plan.objects) {
        auto &delta = allocation[{object.raw_group, object.raw_volume}];
        if (std::ranges::contains(object.actions, PackageImportObjectAction::insert) &&
            !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            ++delta.inserted_object_count;
            object.payload_sectors =
                (node_by_id(packages[object.package_index], object.node_id)->raw_payload.size() + 2047U) / 2048U;
            delta.payload_sectors += object.payload_sectors;
            delta.directory_growth_bytes += 32U;
            ++category_counts[{object.raw_group, object.raw_volume, object.object_type}];
        } else if (std::ranges::contains(object.actions, PackageImportObjectAction::reuse) &&
                   !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            ++delta.reused_object_count;
        }
    }
    for (const auto &[key, count] : category_counts) {
        if (count > 50U) {
            add_conflict(plan, "ISO9660_DIRECTORY_CAPACITY_EXHAUSTED",
                         std::format("ISO category {}/{}/{} exceeds the narrow "
                                     "one-sector directory "
                                     "profile",
                                     std::get<0>(key), std::get<1>(key), std::get<2>(key)));
        }
    }
    std::map<std::string, std::size_t, std::less<>> group_volume_counts;
    for (const auto &[scope, ignored] : planned_scopes) {
        (void)ignored;
        ++group_volume_counts[scope.first];
    }
    for (const auto &[group, count] : group_volume_counts) {
        if (count > 50U)
            add_conflict(plan, "ISO9660_DIRECTORY_CAPACITY_EXHAUSTED",
                         std::format("ISO group '{}' exceeds the narrow "
                                     "one-sector directory profile",
                                     group));
    }

    const auto sectors_for = [](std::uint64_t bytes) { return (bytes + 2047U) / 2048U; };
    std::set<std::string, std::less<>> generated_files;
    std::set<std::string, std::less<>> projected_directories{""};
    std::uint64_t projected_file_sectors{};
    for (const auto &[group, count] : group_volume_counts) {
        generated_files.insert(group + "/0000");
        generated_files.insert(group + "/" + std::format("F{:03}", count + 1U));
        projected_directories.insert(group);
        projected_file_sectors += sectors_for((count + 1U) * 32U);
        projected_file_sectors += sectors_for(16U);
    }
    for (const auto &[group, volumes] : existing_group_volumes)
        generated_files.insert(group + "/" + std::format("F{:03}", volumes.size() + 1U));
    for (const auto &[scope, ignored] : planned_scopes) {
        (void)ignored;
        projected_directories.insert(scope.first + "/" + scope.second);
    }
    for (const auto &[key, count] : category_counts) {
        if (count == 0U)
            continue;
        const auto directory = std::get<0>(key) + "/" + std::get<1>(key) + "/" + std::get<2>(key);
        projected_directories.insert(directory);
        generated_files.insert(directory + "/0000");
        projected_file_sectors += sectors_for(count * 32U);
    }
    for (const auto &item : existing) {
        constexpr std::string_view iso_object_key_prefix{"iso9660:"};
        if (!item.snapshot->key.starts_with(iso_object_key_prefix))
            return std::unexpected{planner_error("existing ISO9660 object has no logical path key")};
        const auto object_path = item.snapshot->key.substr(iso_object_key_prefix.size());
        generated_files.insert(object_path);
        const auto separator = object_path.rfind('/');
        if (separator == std::string::npos)
            return std::unexpected{planner_error("existing ISO9660 object path has no category")};
        generated_files.insert(object_path.substr(0, separator) + "/0000");
        projected_file_sectors += sectors_for(item.snapshot->raw_payload.size());
    }
    for (const auto &object : plan.objects) {
        if (std::ranges::contains(object.actions, PackageImportObjectAction::insert) &&
            !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            const auto *node = node_by_id(packages[object.package_index], object.node_id);
            if (node == nullptr)
                return std::unexpected{planner_error("planned ISO9660 package node is missing")};
            projected_file_sectors += sectors_for(node->raw_payload.size());
        }
    }
    for (const auto &file : iso.files()) {
        if (file.is_directory || generated_files.contains(file.path))
            continue;
        projected_file_sectors += sectors_for(file.size);
        auto parent = file.path;
        while (true) {
            const auto separator = parent.rfind('/');
            if (separator == std::string::npos)
                break;
            parent.resize(separator);
            if (parent.empty())
                break;
            projected_directories.insert(parent);
        }
    }
    const auto projected_image_sectors =
        20U + static_cast<std::uint64_t>(projected_directories.size()) + projected_file_sectors;
    const auto projected_image_size_bytes = projected_image_sectors * 2048U;
    if (projected_image_sectors > std::numeric_limits<std::uint32_t>::max()) {
        add_conflict(plan, "ISO9660_SECTOR_CAPACITY_EXHAUSTED",
                     "ISO image exceeds the narrow 32-bit sector extent profile");
    }
    for (auto &[scope, delta] : allocation) {
        delta.remaining_object_ids = 50U;
        for (const auto &[key, count] : category_counts) {
            if (std::get<0>(key) == scope.first && std::get<1>(key) == scope.second)
                delta.remaining_object_ids =
                    std::min<std::uint64_t>(delta.remaining_object_ids, count > 50U ? 0U : 50U - count);
        }
        delta.projected_image_sectors = projected_image_sectors;
        delta.projected_image_size_bytes = projected_image_size_bytes;
        plan.allocation.push_back(std::move(delta));
    }

    auto final_reader = FileReader::open(target_path);
    if (!final_reader)
        return std::unexpected{final_reader.error()};
    const auto after = package_internal::sha256_reader(**final_reader, cancellation);
    if (!after)
        return std::unexpected{after.error()};
    if (*after != before)
        return std::unexpected{planner_error("target image changed while its import plan was built")};
    std::ranges::sort(plan.conflicts, [](const auto &left, const auto &right) {
        return std::tie(left.code, left.package_index, left.root_index, left.package_id, left.node_id,
                        left.partition_index, left.group_name, left.volume_name, left.raw_group, left.raw_volume,
                        left.message) < std::tie(right.code, right.package_index, right.root_index, right.package_id,
                                                 right.node_id, right.partition_index, right.group_name,
                                                 right.volume_name, right.raw_group, right.raw_volume, right.message);
    });
    plan.plan_id = plan_identity(plan);
    if (const auto verified = verify_package_import_plan(plan); !verified)
        return std::unexpected{verified.error()};
    return plan;
}

bool valid_digest(std::string_view value) {
    return value.size() == 64U && std::ranges::all_of(value, [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

} // namespace

Result<void> verify_package_import_plan(const PackageImportPlan &plan) {
    if (plan.schema_version != "1.0" || !valid_digest(plan.target_snapshot_id) || !valid_digest(plan.policy_digest) ||
        !valid_digest(plan.plan_id)) {
        return std::unexpected{planner_error("package import plan identity fields are invalid")};
    }
    if (std::ranges::any_of(plan.warnings, &PackageIssue::fatal))
        return std::unexpected{planner_error("package import plan contains a fatal warning")};

    std::set<std::tuple<std::uint8_t, std::string, std::string, std::string, std::string>> destination_keys;
    for (const auto &destination : plan.destinations) {
        const auto has_infrastructure = !destination.infrastructure_sfs_ids.empty() ||
                                        destination.infrastructure_clusters != 0U ||
                                        destination.root_directory_growth_bytes != 0U;
        const auto valid_creation =
            !destination.create ||
            (plan.target_kind == MediaKind::sfs && destination.infrastructure_sfs_ids.size() == 6U &&
             destination.infrastructure_clusters == 12U && destination.root_directory_growth_bytes == 32U) ||
            (plan.target_kind == MediaKind::iso9660 && !has_infrastructure);
        if (destination.volume_name.empty() ||
            !destination_keys
                 .emplace(destination.partition_index, destination.group_name, destination.volume_name,
                          destination.raw_group, destination.raw_volume)
                 .second ||
            !valid_creation || (!destination.create && has_infrastructure)) {
            return std::unexpected{planner_error("package import plan contains an invalid destination action")};
        }
    }

    std::set<std::string, std::less<>> action_ids;
    std::map<std::string, const PlannedPackageObject *, std::less<>> actions;
    for (const auto &object : plan.objects) {
        if (plan.valid() && std::ranges::find_if(plan.destinations, [&](const auto &destination) {
                                return destination.partition_index == object.partition_index &&
                                       destination.group_name == object.group_name &&
                                       destination.volume_name == object.volume_name &&
                                       destination.raw_group == object.raw_group &&
                                       destination.raw_volume == object.raw_volume;
                            }) == plan.destinations.end()) {
            return std::unexpected{planner_error("package import action has no planned destination")};
        }
        if (object.package_index >= plan.package_ids.size() ||
            object.package_id != plan.package_ids[object.package_index] || object.actions.empty() ||
            !valid_digest(object.action_id) || !valid_digest(object.normalized_sha256) ||
            !action_ids.emplace(object.action_id).second) {
            return std::unexpected{planner_error("package import plan contains an invalid action")};
        }
        const auto inserts = std::ranges::contains(object.actions, PackageImportObjectAction::insert);
        const auto reuses = std::ranges::contains(object.actions, PackageImportObjectAction::reuse);
        const auto conflicts = std::ranges::contains(object.actions, PackageImportObjectAction::conflict);
        const std::set<PackageImportObjectAction> unique_actions(object.actions.begin(), object.actions.end());
        const auto sorted_programs = std::ranges::is_sorted(object.target_program_numbers);
        const std::set<std::uint8_t> unique_programs(object.target_program_numbers.begin(),
                                                     object.target_program_numbers.end());
        if (unique_actions.size() != object.actions.size() || (inserts && reuses) ||
            (plan.valid() && !inserts && !reuses) || (plan.valid() && conflicts) ||
            (!sorted_programs || unique_programs.size() != object.target_program_numbers.size()) ||
            std::ranges::any_of(object.target_program_numbers,
                                [](const auto number) { return number < 1U || number > 128U; }) ||
            (object.object_type != "SBNK" && (!object.target_program_numbers.empty() || object.target_grouped)) ||
            (plan.target_kind == MediaKind::iso9660
                 ? (object.payload_clusters != 0U || object.continuation_clusters != 0U ||
                    (inserts && !conflicts && object.payload_sectors == 0U))
                 : object.payload_sectors != 0U) ||
            (inserts && !conflicts &&
             ((plan.target_kind == MediaKind::sfs && !object.target_sfs_id) ||
              (object.object_type == "SMPL" && !object.target_link_id))) ||
            (reuses && !conflicts && !object.existing_object_key && !object.canonical_action_id)) {
            return std::unexpected{planner_error("package import plan action decision is "
                                                 "incomplete or contradictory")};
        }
        actions.emplace(object.action_id, &object);
    }
    for (const auto &object : plan.objects) {
        if (!object.canonical_action_id)
            continue;
        const auto canonical = actions.find(*object.canonical_action_id);
        if (canonical == actions.end() ||
            !std::ranges::contains(canonical->second->actions, PackageImportObjectAction::insert) ||
            object.partition_index != canonical->second->partition_index ||
            object.group_name != canonical->second->group_name ||
            object.volume_name != canonical->second->volume_name || object.raw_group != canonical->second->raw_group ||
            object.raw_volume != canonical->second->raw_volume ||
            object.object_type != canonical->second->object_type ||
            object.destination_name != canonical->second->destination_name ||
            object.normalized_sha256 != canonical->second->normalized_sha256 ||
            object.target_sfs_id != canonical->second->target_sfs_id ||
            object.target_link_id != canonical->second->target_link_id ||
            object.target_program_numbers != canonical->second->target_program_numbers ||
            object.target_grouped != canonical->second->target_grouped) {
            return std::unexpected{planner_error("package import plan canonical reuse binding is invalid")};
        }
    }
    for (const auto &allocation : plan.allocation) {
        const auto iso = plan.target_kind == MediaKind::iso9660;
        if ((iso && (allocation.payload_clusters != 0U || allocation.continuation_clusters != 0U ||
                     allocation.projected_image_sectors == 0U ||
                     allocation.projected_image_size_bytes != allocation.projected_image_sectors * 2048U)) ||
            (!iso && (allocation.payload_sectors != 0U || allocation.projected_image_sectors != 0U ||
                      allocation.projected_image_size_bytes != 0U))) {
            return std::unexpected{planner_error("package import plan allocation units do not "
                                                 "match the target media")};
        }
    }
    if (plan.plan_id != plan_identity(plan))
        return std::unexpected{planner_error("package import plan identity does not match its actions")};
    return {};
}

Result<PackageImportPlan> plan_package_import(const std::filesystem::path &target_path,
                                              std::span<const PortablePackage> packages,
                                              const PackageImportRequest &request,
                                              const CancellationToken &cancellation) {
    if (packages.empty())
        return std::unexpected{planner_error("package import requires at least one package")};
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};

    auto target_reader = FileReader::open(target_path);
    if (!target_reader)
        return std::unexpected{target_reader.error()};
    const auto before = package_internal::sha256_reader(**target_reader, cancellation);
    if (!before)
        return std::unexpected{before.error()};
    auto target = open_media(target_path, cancellation);
    if (!target)
        return std::unexpected{target.error()};

    PackageImportPlan plan;
    plan.schema_version = "1.0";
    plan.target_kind = target->kind();
    plan.target_snapshot_id = package_internal::hex_digest(*before);
    plan.policy_digest = policy_digest(request.policy);
    for (std::size_t package_index = 0; package_index < packages.size(); ++package_index) {
        const auto &package = packages[package_index];
        if (const auto verified = verify_portable_package(package); !verified)
            return std::unexpected{verified.error()};
        plan.package_ids.push_back(package.package_id);
        for (const auto &issue : package.issues) {
            if (!issue.fatal)
                plan.warnings.push_back(issue);
        }
    }

    if (target->kind() == MediaKind::fat12_floppy) {
        return plan_fat12_import(target_path, packages, request, *target, std::move(plan), *before, cancellation);
    }
    if (target->kind() == MediaKind::iso9660) {
        return plan_iso9660_import(target_path, packages, request, *target, std::move(plan), *before, cancellation);
    }
    if (target->kind() != MediaKind::sfs) {
        add_conflict(plan, "TARGET_ADAPTER_UNSUPPORTED",
                     "portable package import is not yet implemented for this "
                     "target profile");
        plan.plan_id = plan_identity(plan);
        return plan;
    }
    const auto &container = std::get<Container>(target->storage());
    auto catalog = build_object_catalog(container, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    for (const auto &issue : catalog->issues) {
        add_conflict(plan, issue.code, issue.message);
        auto &conflict = plan.conflicts.back();
        conflict.partition_index = issue.partition.value;
    }
    for (const auto &partition : container.partitions()) {
        if (partition.allocation.invalid_extent_record_count != 0U ||
            !partition.allocation.stored_not_reconstructed.empty() ||
            !partition.allocation.reconstructed_not_stored.empty()) {
            add_conflict(plan, "SFS_ALLOCATION_INVALID",
                         "target partition allocation is not safe for package planning");
            plan.conflicts.back().partition_index = partition.index.value;
        }
    }

    const auto volumes = sfs_volumes(container);
    const auto existing = existing_objects(*catalog);
    std::map<std::uint8_t, PartitionCapacity> capacities;
    for (const auto &partition : container.partitions())
        capacities.emplace(partition.index.value, partition_capacity(partition, *catalog));

    std::map<std::pair<std::size_t, std::size_t>, const PackageRootDestination *> destinations;
    for (const auto &destination : request.root_destinations) {
        if (destination.package_index >= packages.size() ||
            destination.root_index >=
                packages[std::min(destination.package_index, packages.size() - 1U)].roots.size()) {
            add_conflict(plan, "DESTINATION_ROOT_INVALID", "root destination references a missing package or root",
                         &destination);
            continue;
        }
        const auto key = std::pair{destination.package_index, destination.root_index};
        if (!destinations.emplace(key, &destination).second) {
            add_conflict(plan, "DESTINATION_ROOT_DUPLICATE", "package root has more than one destination mapping",
                         &destination, &packages[destination.package_index]);
        }
    }
    for (std::size_t package_index = 0; package_index < packages.size(); ++package_index) {
        for (std::size_t root_index = 0; root_index < packages[package_index].roots.size(); ++root_index) {
            if (!destinations.contains({package_index, root_index})) {
                PackageRootDestination missing;
                missing.package_index = package_index;
                missing.root_index = root_index;
                add_conflict(plan, "DESTINATION_ROOT_MISSING", "every package root requires one explicit destination",
                             &missing, &packages[package_index]);
            }
        }
    }

    std::map<std::pair<std::size_t, std::string>, std::string> renames;
    for (const auto &rename : request.policy.renames) {
        if (rename.package_index >= packages.size() ||
            node_by_id(packages[std::min(rename.package_index, packages.size() - 1U)], rename.node_id) == nullptr) {
            add_conflict(plan, "RENAME_NODE_INVALID", "rename references a missing package node");
            continue;
        }
        const auto key = std::pair{rename.package_index, rename.node_id};
        if (!valid_sfs_name(rename.destination_name)) {
            add_conflict(plan, "RENAME_NAME_INVALID", "SFS destination names must contain 1 to 16 ASCII bytes");
        } else if (!renames.emplace(key, rename.destination_name).second) {
            add_conflict(plan, "RENAME_NODE_DUPLICATE", "package node has more than one rename");
        }
    }

    std::vector<Candidate> candidates;
    std::map<DestinationKey, bool> destination_creation;
    for (const auto &[key, destination] : destinations) {
        const auto &[package_index, root_index] = key;
        const auto &package = packages[package_index];
        if (!destination->partition_index || destination->volume_name.empty() || !destination->group_name.empty() ||
            !destination->raw_group.empty() || !destination->raw_volume.empty()) {
            add_conflict(plan, "SFS_DESTINATION_INVALID",
                         "SFS destinations require a partition and volume but "
                         "no group or raw ISO "
                         "identifiers",
                         destination, &package);
            continue;
        }
        if (!capacities.contains(*destination->partition_index)) {
            add_conflict(plan, "SFS_DESTINATION_PARTITION_MISSING", "SFS destination partition does not exist",
                         destination, &package);
            continue;
        }
        const DestinationKey destination_key{*destination->partition_index, destination->volume_name};
        const auto destination_exists = volumes.contains(destination_key);
        if ((destination_exists && destination->create_destination) ||
            (!destination_exists && !destination->create_destination)) {
            add_conflict(plan, destination_exists ? "SFS_DESTINATION_ALREADY_EXISTS" : "SFS_DESTINATION_MISSING",
                         destination_exists ? "SFS destination creation requested an existing volume"
                                            : "SFS destination volume does not exist",
                         destination, &package);
            continue;
        }
        const auto [creation, inserted] =
            destination_creation.emplace(destination_key, destination->create_destination);
        if (!inserted && creation->second != destination->create_destination) {
            add_conflict(plan, "SFS_DESTINATION_POLICY_CONFLICT",
                         "package roots disagree about destination volume creation", destination, &package);
            continue;
        }
        const auto closure = root_closure(package, root_index);
        std::map<std::string, std::string, std::less<>> destination_names;
        for (const auto *node : closure) {
            auto name = node->name;
            if (const auto renamed = renames.find({package_index, node->node_id}); renamed != renames.end())
                name = renamed->second;
            destination_names.emplace(node->node_id, std::move(name));
        }
        for (const auto *node : closure) {
            auto name = destination_names.at(node->node_id);
            if (!valid_sfs_name(name)) {
                add_conflict(plan, "SFS_OBJECT_NAME_INVALID", "SFS object names must contain 1 to 16 ASCII bytes",
                             destination, &package, node);
                continue;
            }
            if (node->object_type == "PROG") {
                PlannedPackageObject program;
                program.destination_name = name;
                if (const auto number = planned_program_number(program); !number) {
                    add_conflict(plan, "SFS_PROGRAM_SLOT_INVALID", number.error().message, destination, &package, node);
                    continue;
                }
            }
            auto normalized = projected_normalized_sha256(package, *node, destination_names);
            if (!normalized)
                return std::unexpected{normalized.error()};
            candidates.push_back({&package, node, destination, std::move(name), std::move(*normalized)});
        }
    }

    std::map<std::uint8_t, std::size_t> new_volume_counts;
    for (const auto &[key, create] : destination_creation) {
        PlannedPackageDestination planned;
        planned.partition_index = key.first;
        planned.volume_name = key.second;
        planned.create = create;
        if (create) {
            auto capacity = capacities.find(key.first);
            if (capacity == capacities.end()) {
                add_conflict(plan, "SFS_DESTINATION_PARTITION_MISSING", "SFS destination partition does not exist");
            } else if (capacity->second.free_ids.size() - capacity->second.next_id < 6U) {
                add_conflict(plan, "SFS_OBJECT_ID_EXHAUSTED",
                             "partition lacks six SFS records for destination "
                             "volume scaffolding");
            } else {
                for (std::size_t index = 0; index < 6U; ++index) {
                    planned.infrastructure_sfs_ids.push_back(capacity->second.free_ids[capacity->second.next_id++]);
                }
                bool cluster_failure{};
                for (std::size_t index = 0; index < 6U; ++index) {
                    const auto reserved = reserve_clusters(capacity->second, 2U);
                    if (!reserved) {
                        cluster_failure = true;
                        break;
                    }
                    planned.infrastructure_clusters += reserved->first + reserved->second;
                }
                if (cluster_failure) {
                    add_conflict(plan, "SFS_CLUSTER_EXHAUSTED",
                                 "partition lacks clusters for destination "
                                 "volume scaffolding");
                }
                planned.root_directory_growth_bytes = 32U;
                ++new_volume_counts[key.first];
            }
        }
        plan.destinations.push_back(std::move(planned));
    }
    for (const auto &[partition_index, count] : new_volume_counts) {
        const auto partition =
            std::ranges::find(container.partitions(), PartitionIndex{partition_index}, &Partition::index);
        if (partition == container.partitions().end())
            continue;
        const auto root = std::ranges::find(partition->records, SfsId{1}, &IndexRecord::sfs_id);
        if (root == partition->records.end()) {
            add_conflict(plan, "SFS_ROOT_DIRECTORY_MISSING",
                         "partition root directory is unavailable for "
                         "destination creation");
            continue;
        }
        std::uint64_t root_capacity{};
        for (const auto &extent : root->extents)
            root_capacity += static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
        if (root->data_size + count * 32U > root_capacity) {
            add_conflict(plan, "SFS_ROOT_DIRECTORY_CAPACITY_EXHAUSTED",
                         "partition root directory cannot contain all planned "
                         "destination volumes");
        }
    }
    std::ranges::sort(candidates, [](const Candidate &left, const Candidate &right) {
        return std::tuple{*left.destination->partition_index,
                          left.destination->volume_name,
                          type_rank(left.node->object_type),
                          left.destination_name,
                          left.projected_normalized_sha256,
                          left.package->package_id,
                          left.destination->package_index,
                          left.destination->root_index,
                          left.node->node_id} < std::tuple{*right.destination->partition_index,
                                                           right.destination->volume_name,
                                                           type_rank(right.node->object_type),
                                                           right.destination_name,
                                                           right.projected_normalized_sha256,
                                                           right.package->package_id,
                                                           right.destination->package_index,
                                                           right.destination->root_index,
                                                           right.node->node_id};
    });

    std::map<std::tuple<std::uint8_t, std::string, std::string, std::string>, std::size_t> planned_names;
    for (const auto &candidate : candidates) {
        PlannedPackageObject object;
        object.action_id = action_identity(candidate);
        object.package_index = candidate.destination->package_index;
        object.root_index = candidate.destination->root_index;
        object.package_id = candidate.package->package_id;
        object.node_id = candidate.node->node_id;
        object.object_type = candidate.node->object_type;
        object.source_name = candidate.node->name;
        object.destination_name = candidate.destination_name;
        object.normalized_sha256 = candidate.projected_normalized_sha256;
        object.partition_index = *candidate.destination->partition_index;
        object.group_name = candidate.destination->group_name;
        object.volume_name = candidate.destination->volume_name;
        if (object.source_name != object.destination_name)
            object.actions.push_back(PackageImportObjectAction::rename);

        std::vector<const ExistingObject *> matches;
        for (const auto &item : existing) {
            if (!item.snapshot->placement || item.snapshot->partition.value != object.partition_index ||
                item.snapshot->placement->volume_name != object.volume_name ||
                item.snapshot->object.header.raw_type != object.object_type ||
                item.snapshot->object.header.name != object.destination_name) {
                continue;
            }
            matches.push_back(&item);
        }
        if (matches.size() > 1U) {
            mark_conflict(object);
            add_conflict(plan, "SFS_TARGET_NAME_AMBIGUOUS",
                         "destination contains multiple objects with the same "
                         "type and name",
                         candidate.destination, candidate.package, candidate.node);
        } else if (matches.size() == 1U) {
            if (matches.front()->normalized_sha256 == candidate.projected_normalized_sha256) {
                object.actions.push_back(PackageImportObjectAction::reuse);
                object.existing_object_key = matches.front()->snapshot->key;
                object.target_sfs_id = matches.front()->snapshot->sfs_id.value;
                object.target_link_id = matches.front()->link_id;
            } else {
                mark_conflict(object);
                add_conflict(plan, "SFS_NAME_CONFLICT",
                             "destination already contains the same object "
                             "name with different content",
                             candidate.destination, candidate.package, candidate.node);
            }
        } else {
            const auto name_key =
                std::tuple{object.partition_index, object.volume_name, object.object_type, object.destination_name};
            if (const auto found = planned_names.find(name_key); found != planned_names.end()) {
                const auto &canonical = plan.objects[found->second];
                if (canonical.normalized_sha256 == object.normalized_sha256 &&
                    !std::ranges::contains(canonical.actions, PackageImportObjectAction::conflict)) {
                    object.actions.push_back(PackageImportObjectAction::reuse);
                    object.canonical_action_id = canonical.action_id;
                    object.target_sfs_id = canonical.target_sfs_id;
                    object.target_link_id = canonical.target_link_id;
                } else {
                    mark_conflict(object);
                    add_conflict(plan, "SFS_NAME_CONFLICT",
                                 "incoming package roots assign different "
                                 "content to the same object name",
                                 candidate.destination, candidate.package, candidate.node);
                    mark_conflict(plan.objects[found->second]);
                    PackageImportConflict canonical_conflict;
                    canonical_conflict.code = "SFS_NAME_CONFLICT";
                    canonical_conflict.message = "incoming package roots assign different content to "
                                                 "the same object name";
                    canonical_conflict.package_index = canonical.package_index;
                    canonical_conflict.root_index = canonical.root_index;
                    canonical_conflict.package_id = canonical.package_id;
                    canonical_conflict.node_id = canonical.node_id;
                    canonical_conflict.partition_index = canonical.partition_index;
                    canonical_conflict.group_name = canonical.group_name;
                    canonical_conflict.volume_name = canonical.volume_name;
                    plan.conflicts.push_back(std::move(canonical_conflict));
                }
            } else {
                if (!candidate.node->relocations.empty())
                    object.actions.push_back(PackageImportObjectAction::relocate);
                object.actions.push_back(PackageImportObjectAction::insert);
                planned_names.emplace(name_key, plan.objects.size());
            }
        }
        plan.objects.push_back(std::move(object));
    }

    std::map<std::tuple<std::uint8_t, std::string, std::string>, std::vector<std::size_t>> category_insertions;
    for (std::size_t index = 0; index < plan.objects.size(); ++index) {
        auto &object = plan.objects[index];
        if (!std::ranges::contains(object.actions, PackageImportObjectAction::insert) ||
            std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            continue;
        }
        auto &capacity = capacities.at(object.partition_index);
        if (capacity.next_id >= capacity.free_ids.size()) {
            mark_conflict(object);
            add_conflict(plan, "SFS_OBJECT_ID_EXHAUSTED",
                         "partition has no free SFS object record for the "
                         "planned import");
            auto &conflict = plan.conflicts.back();
            conflict.package_index = object.package_index;
            conflict.root_index = object.root_index;
            conflict.package_id = object.package_id;
            conflict.node_id = object.node_id;
            conflict.partition_index = object.partition_index;
            conflict.volume_name = object.volume_name;
            continue;
        }
        object.target_sfs_id = capacity.free_ids[capacity.next_id++];
        if (object.object_type == "SMPL") {
            while (capacity.used_link_ids.contains(capacity.next_link_id))
                capacity.next_link_id += 0x100U;
            object.target_link_id = capacity.next_link_id;
            capacity.used_link_ids.insert(capacity.next_link_id);
            capacity.next_link_id += 0x100U;
        }
        const auto *package_node = node_by_id(packages[object.package_index], object.node_id);
        const auto clusters =
            std::max<std::uint32_t>(2U, static_cast<std::uint32_t>((package_node->raw_payload.size() + 1023U) / 1024U));
        const auto reserved = reserve_clusters(capacity, clusters);
        if (!reserved) {
            mark_conflict(object);
            add_conflict(plan, "SFS_CLUSTER_EXHAUSTED",
                         "partition has insufficient clusters for the planned "
                         "object payload");
            auto &conflict = plan.conflicts.back();
            conflict.package_index = object.package_index;
            conflict.root_index = object.root_index;
            conflict.package_id = object.package_id;
            conflict.node_id = object.node_id;
            conflict.partition_index = object.partition_index;
            conflict.volume_name = object.volume_name;
            continue;
        }
        object.payload_clusters = reserved->first;
        object.continuation_clusters = reserved->second;
        category_insertions[{object.partition_index, object.volume_name, object.object_type}].push_back(index);
    }

    for (const auto &[key, indices] : category_insertions) {
        const auto &[partition_index, volume_name, category_name] = key;
        const auto planned_destination = destination_creation.find({partition_index, volume_name});
        if (planned_destination != destination_creation.end() && planned_destination->second) {
            const auto growth = indices.size() * 32U;
            if (64U + growth > 2048U) {
                for (const auto index : indices)
                    mark_conflict(plan.objects[index]);
                add_conflict(plan, "SFS_DIRECTORY_CAPACITY_EXHAUSTED",
                             "new object category directory cannot contain all "
                             "planned objects");
                auto &conflict = plan.conflicts.back();
                conflict.partition_index = partition_index;
                conflict.volume_name = volume_name;
            }
            continue;
        }
        const auto volume = volumes.find({partition_index, volume_name});
        const auto category = volume == volumes.end()
                                  ? std::map<std::string, const IndexRecord *, std::less<>>::const_iterator{}
                                  : volume->second.categories.find(category_name);
        if (volume == volumes.end() || category == volume->second.categories.end()) {
            for (const auto index : indices)
                mark_conflict(plan.objects[index]);
            add_conflict(plan, "SFS_CATEGORY_MISSING",
                         "destination volume does not contain the required "
                         "object category");
            auto &conflict = plan.conflicts.back();
            conflict.partition_index = partition_index;
            conflict.volume_name = volume_name;
            continue;
        }
        std::uint64_t capacity_bytes{};
        for (const auto &extent : category->second->extents)
            capacity_bytes += static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
        const auto growth = indices.size() * 32U;
        if (category->second->data_size + growth > capacity_bytes) {
            for (const auto index : indices)
                mark_conflict(plan.objects[index]);
            add_conflict(plan, "SFS_DIRECTORY_CAPACITY_EXHAUSTED",
                         "object category directory has insufficient retained "
                         "extent capacity");
            auto &conflict = plan.conflicts.back();
            conflict.partition_index = partition_index;
            conflict.volume_name = volume_name;
        }
    }

    std::map<std::string, const PlannedPackageObject *, std::less<>> actions_by_id;
    for (const auto &object : plan.objects)
        actions_by_id.emplace(object.action_id, &object);
    for (auto &object : plan.objects) {
        if (!object.canonical_action_id)
            continue;
        const auto canonical = actions_by_id.find(*object.canonical_action_id);
        if (canonical == actions_by_id.end()) {
            mark_conflict(object);
            add_conflict(plan, "PLANNED_CANONICAL_OBJECT_MISSING",
                         "reused incoming object has no canonical planned allocation");
            continue;
        }
        object.target_sfs_id = canonical->second->target_sfs_id;
        object.target_link_id = canonical->second->target_link_id;
        if (std::ranges::contains(canonical->second->actions, PackageImportObjectAction::conflict))
            mark_conflict(object);
    }

    struct SbnkTargetMetadata {
        std::set<std::uint8_t> program_numbers;
        bool grouped{};
    };
    using PhysicalObjectKey = std::pair<std::uint8_t, std::uint32_t>;
    std::map<PhysicalObjectKey, SbnkTargetMetadata> sbnk_metadata;
    for (const auto &object : plan.objects) {
        if (object.object_type != "SBNK" || !object.target_sfs_id)
            continue;
        auto &metadata = sbnk_metadata[{object.partition_index, *object.target_sfs_id}];
        if (!object.existing_object_key)
            continue;
        const auto found = std::ranges::find_if(existing, [&](const ExistingObject &candidate) {
            return candidate.snapshot->key == *object.existing_object_key;
        });
        if (found == existing.end())
            continue;
        if (const auto *bank = std::get_if<CurrentSbnk>(&found->snapshot->object.payload)) {
            metadata.program_numbers.insert(bank->linked_program_numbers.begin(), bank->linked_program_numbers.end());
            metadata.grouped = (bank->sample_flags & 1U) != 0U;
        }
    }
    for (const auto &owner : plan.objects) {
        if (std::ranges::contains(owner.actions, PackageImportObjectAction::conflict))
            continue;
        const auto &package = packages[owner.package_index];
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != owner.node_id ||
                (edge.role != "SBAC_SLOT_TO_SBNK" && edge.role != "PROG_ASSIGNMENT_TO_SBNK")) {
                continue;
            }
            const auto *target_action = planned_node(plan, owner, edge.target_node_id);
            if (target_action == nullptr || target_action->object_type != "SBNK" || !target_action->target_sfs_id) {
                continue;
            }
            auto &metadata = sbnk_metadata[{target_action->partition_index, *target_action->target_sfs_id}];
            if (edge.role == "SBAC_SLOT_TO_SBNK") {
                metadata.grouped = true;
            } else {
                auto number = planned_program_number(owner);
                if (!number)
                    return std::unexpected{number.error()};
                metadata.program_numbers.insert(*number);
            }
        }
    }
    for (auto &object : plan.objects) {
        if (object.object_type != "SBNK" || !object.target_sfs_id)
            continue;
        const auto metadata = sbnk_metadata.find({object.partition_index, *object.target_sfs_id});
        if (metadata == sbnk_metadata.end())
            continue;
        object.target_program_numbers.assign(metadata->second.program_numbers.begin(),
                                             metadata->second.program_numbers.end());
        object.target_grouped = metadata->second.grouped;
    }

    if (plan.conflicts.empty()) {
        for (auto &object : plan.objects) {
            const auto *node = node_by_id(packages[object.package_index], object.node_id);
            if (node == nullptr)
                return std::unexpected{planner_error("planned package node is missing")};
            auto context = relocation_context(packages[object.package_index], plan, object);
            if (!context)
                return std::unexpected{context.error()};
            auto relocated = package_internal::relocate_package_node(packages[object.package_index], *node, *context);
            if (!relocated)
                return std::unexpected{relocated.error()};
            auto decoded = decode_object(*relocated);
            if (!decoded)
                return std::unexpected{decoded.error()};
            auto profile = package_internal::build_relocation_profile(*decoded, *relocated);
            if (!profile)
                return std::unexpected{profile.error()};
            const auto normalized = package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
            if (normalized != object.normalized_sha256) {
                return std::unexpected{planner_error("planned package relocation changed "
                                                     "normalized object identity")};
            }
            if (std::ranges::contains(object.actions, PackageImportObjectAction::insert)) {
                if (*relocated != node->raw_payload &&
                    !std::ranges::contains(object.actions, PackageImportObjectAction::relocate)) {
                    object.actions.push_back(PackageImportObjectAction::relocate);
                }
                continue;
            }
            if (!object.existing_object_key)
                continue;
            const auto found = std::ranges::find_if(existing, [&](const ExistingObject &candidate) {
                return candidate.snapshot->key == *object.existing_object_key;
            });
            if (found == existing.end())
                return std::unexpected{planner_error("planned existing package object is missing")};
            if (*relocated == found->snapshot->raw_payload)
                continue;
            if (object.object_type != "SBNK") {
                return std::unexpected{planner_error("existing package object relocation fields "
                                                     "do not match the projected target")};
            }
            if (!std::ranges::contains(object.actions, PackageImportObjectAction::relocate))
                object.actions.push_back(PackageImportObjectAction::relocate);
        }
    }

    std::map<DestinationKey, PackageAllocationDelta> allocation;
    for (const auto &destination : plan.destinations) {
        auto &delta = allocation[{destination.partition_index, destination.volume_name}];
        delta.partition_index = destination.partition_index;
        delta.group_name = destination.group_name;
        delta.volume_name = destination.volume_name;
        delta.directory_growth_bytes += destination.root_directory_growth_bytes;
    }
    for (const auto &object : plan.objects) {
        auto &delta = allocation[{object.partition_index, object.volume_name}];
        delta.partition_index = object.partition_index;
        delta.group_name = object.group_name;
        delta.volume_name = object.volume_name;
        if (std::ranges::contains(object.actions, PackageImportObjectAction::insert) &&
            !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            ++delta.inserted_object_count;
            delta.payload_clusters += object.payload_clusters;
            delta.continuation_clusters += object.continuation_clusters;
            delta.directory_growth_bytes += 32U;
        } else if (std::ranges::contains(object.actions, PackageImportObjectAction::reuse) &&
                   !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            ++delta.reused_object_count;
        }
    }
    for (auto &[key, delta] : allocation) {
        const auto &capacity = capacities.at(key.first);
        delta.remaining_object_ids = capacity.free_ids.size() - capacity.next_id;
        delta.remaining_clusters = remaining_clusters(capacity);
        plan.allocation.push_back(std::move(delta));
    }

    auto final_reader = FileReader::open(target_path);
    if (!final_reader)
        return std::unexpected{final_reader.error()};
    const auto after = package_internal::sha256_reader(**final_reader, cancellation);
    if (!after)
        return std::unexpected{after.error()};
    if (*after != *before)
        return std::unexpected{planner_error("target image changed while its import plan was built")};

    std::ranges::sort(plan.conflicts, [](const auto &left, const auto &right) {
        return std::tie(left.code, left.package_index, left.root_index, left.package_id, left.node_id,
                        left.partition_index, left.group_name, left.volume_name, left.message) <
               std::tie(right.code, right.package_index, right.root_index, right.package_id, right.node_id,
                        right.partition_index, right.group_name, right.volume_name, right.message);
    });
    plan.plan_id = plan_identity(plan);
    if (const auto verified = verify_package_import_plan(plan); !verified)
        return std::unexpected{verified.error()};
    return plan;
}

} // namespace axk
