#include "package_import_support.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <tuple>

namespace axk::package_import_internal {

Result<PackageImportPlan> plan_sfs_import(std::shared_ptr<const RandomAccessReader> target_reader,
                                          std::span<const PortablePackage> packages,
                                          const PackageImportRequest &request, const MediaContainer *target,
                                          const RetainedPackageImportTarget *retained_session, PackageImportPlan plan,
                                          const std::optional<package_internal::Sha256Digest> &before,
                                          bool revalidate_target, const CancellationToken &cancellation) {
    const auto &container = std::get<Container>(target->storage());
    std::optional<ObjectCatalog> opened_catalog;
    std::vector<const ObjectSnapshot *> opened_catalog_objects;
    std::span<const ObjectSnapshot *const> catalog_objects;
    std::span<const CatalogIssue> catalog_issues;
    if (retained_session != nullptr) {
        catalog_objects = retained_session->catalog_objects;
        catalog_issues = retained_session->catalog_issues;
    } else {
        auto catalog = build_object_catalog(container, 64U * 1024U * 1024U, cancellation);
        if (!catalog)
            return std::unexpected{catalog.error()};
        opened_catalog.emplace(std::move(*catalog));
        opened_catalog_objects.reserve(opened_catalog->objects.size());
        for (const auto &object : opened_catalog->objects)
            opened_catalog_objects.push_back(&object);
        catalog_objects = opened_catalog_objects;
        catalog_issues = opened_catalog->issues;
    }
    for (const auto &issue : catalog_issues) {
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
    auto existing =
        retained_session != nullptr ? retained_existing_objects(catalog_objects) : existing_objects(*opened_catalog);
    using ExistingNameKey = std::tuple<std::uint8_t, std::string, std::string, std::string>;
    std::map<ExistingNameKey, std::vector<std::size_t>> existing_by_name;
    std::map<std::string, std::size_t, std::less<>> existing_by_key;
    for (std::size_t index = 0U; index < existing.size(); ++index) {
        const auto &item = existing[index];
        existing_by_key.emplace(item.snapshot->key, index);
        if (!item.snapshot->placement)
            continue;
        existing_by_name[{item.snapshot->partition.value, item.snapshot->placement->volume_name,
                          item.snapshot->object.header.raw_type, item.snapshot->object.header.name}]
            .push_back(index);
    }
    std::map<std::uint8_t, PartitionCapacity> capacities;
    for (const auto &partition : container.partitions()) {
        capacities.emplace(partition.index.value, retained_session != nullptr
                                                      ? partition_capacity(partition, catalog_objects)
                                                      : partition_capacity(partition, *opened_catalog));
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

    std::map<std::pair<std::uint8_t, std::uint32_t>, ClusterReservation> infrastructure_layouts;
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
                for (std::size_t index = 0; index < planned.infrastructure_sfs_ids.size(); ++index) {
                    const auto reserved = reserve_clusters(capacity->second, 2U);
                    if (!reserved) {
                        cluster_failure = true;
                        break;
                    }
                    planned.infrastructure_clusters += reserved->payload_clusters + reserved->continuation_clusters;
                    infrastructure_layouts.emplace(
                        std::pair{planned.partition_index, planned.infrastructure_sfs_ids[index]}, *reserved);
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

        const auto matches = existing_by_name.find(
            {object.partition_index, object.volume_name, object.object_type, object.destination_name});
        const auto match_count = matches == existing_by_name.end() ? 0U : matches->second.size();
        if (match_count > 1U) {
            mark_conflict(object);
            add_conflict(plan, "SFS_TARGET_NAME_AMBIGUOUS",
                         "destination contains multiple objects with the same "
                         "type and name",
                         candidate.destination, candidate.package, candidate.node);
        } else if (match_count == 1U) {
            auto &match = existing[matches->second.front()];
            if (auto loaded = ensure_existing_identity(
                    match, container, retained_session ? retained_session->stats : nullptr, cancellation);
                !loaded)
                return std::unexpected{loaded.error()};
            if (match.normalized_sha256 == candidate.projected_normalized_sha256) {
                object.actions.push_back(PackageImportObjectAction::reuse);
                object.existing_object_key = match.snapshot->key;
                object.target_sfs_id = match.snapshot->sfs_id.value;
                object.target_wave_data_reference_value = match.wave_data_reference_value;
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
                    object.target_wave_data_reference_value = canonical.target_wave_data_reference_value;
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

    using CategoryKey = std::tuple<std::uint8_t, std::string, std::string>;
    std::map<CategoryKey, std::vector<std::size_t>> category_insertions;
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
            while (capacity.used_wave_data_reference_values.contains(capacity.next_wave_data_reference_value))
                capacity.next_wave_data_reference_value += 0x100U;
            object.target_wave_data_reference_value = capacity.next_wave_data_reference_value;
            capacity.used_wave_data_reference_values.insert(capacity.next_wave_data_reference_value);
            capacity.next_wave_data_reference_value += 0x100U;
        }
        category_insertions[{object.partition_index, object.volume_name, object.object_type}].push_back(index);
    }

    std::map<DestinationKey, std::pair<std::uint64_t, std::uint64_t>> directory_allocations;
    for (const auto &[key, indices] : category_insertions) {
        const auto &[partition_index, volume_name, category_name] = key;
        const auto planned_destination = destination_creation.find({partition_index, volume_name});
        std::span<const Extent> existing_extents;
        std::size_t existing_continuation_clusters{};
        std::uint64_t existing_data_size{};
        std::vector<Extent> new_destination_extents;
        if (planned_destination != destination_creation.end() && planned_destination->second) {
            constexpr std::array<std::string_view, 5> category_names{"SMPL", "SBNK", "SBAC", "SEQU", "PROG"};
            const auto category_index = std::ranges::find(category_names, category_name);
            const auto destination = std::ranges::find_if(plan.destinations, [&](const auto &candidate) {
                return candidate.partition_index == partition_index && candidate.volume_name == volume_name;
            });
            if (category_index == category_names.end() || destination == plan.destinations.end() ||
                destination->infrastructure_sfs_ids.size() != 6U) {
                for (const auto index : indices)
                    mark_conflict(plan.objects[index]);
                add_conflict(plan, "SFS_CATEGORY_MISSING",
                             "new destination volume does not contain the required object category");
                auto &conflict = plan.conflicts.back();
                conflict.partition_index = partition_index;
                conflict.volume_name = volume_name;
                continue;
            }
            const auto category_offset = static_cast<std::size_t>(category_index - category_names.begin()) + 1U;
            const auto layout =
                infrastructure_layouts.find({partition_index, destination->infrastructure_sfs_ids[category_offset]});
            if (layout == infrastructure_layouts.end()) {
                for (const auto index : indices)
                    mark_conflict(plan.objects[index]);
                add_conflict(plan, "SFS_CATEGORY_MISSING", "new destination category allocation is unavailable");
                auto &conflict = plan.conflicts.back();
                conflict.partition_index = partition_index;
                conflict.volume_name = volume_name;
                continue;
            }
            new_destination_extents = layout->second.extents;
            existing_extents = new_destination_extents;
            existing_continuation_clusters = static_cast<std::size_t>(layout->second.continuation_clusters);
            existing_data_size = 64U;
        } else {
            const auto volume = volumes.find({partition_index, volume_name});
            const auto category = volume == volumes.end()
                                      ? std::map<std::string, const IndexRecord *, std::less<>>::const_iterator{}
                                      : volume->second.categories.find(category_name);
            if (volume == volumes.end() || category == volume->second.categories.end()) {
                for (const auto index : indices)
                    mark_conflict(plan.objects[index]);
                add_conflict(plan, "SFS_CATEGORY_MISSING",
                             "destination volume does not contain the required object category");
                auto &conflict = plan.conflicts.back();
                conflict.partition_index = partition_index;
                conflict.volume_name = volume_name;
                continue;
            }
            existing_extents = category->second->extents;
            existing_continuation_clusters = category->second->continuation_clusters.size();
            existing_data_size = category->second->data_size;
        }

        std::uint64_t capacity_clusters{};
        for (const auto &extent : existing_extents)
            capacity_clusters += extent.cluster_count;
        const auto required_size = existing_data_size + indices.size() * 32U;
        const auto required_clusters = (required_size + 1023U) / 1024U;
        if (required_clusters <= capacity_clusters)
            continue;
        const auto added_clusters = static_cast<std::uint32_t>(required_clusters - capacity_clusters);
        auto &capacity = capacities.at(partition_index);
        const auto reserved =
            reserve_directory_growth(capacity, existing_extents, existing_continuation_clusters, added_clusters);
        if (!reserved) {
            for (const auto index : indices)
                mark_conflict(plan.objects[index]);
            add_conflict(plan, "SFS_CLUSTER_EXHAUSTED",
                         "partition has insufficient clusters to grow an object category directory");
            auto &conflict = plan.conflicts.back();
            conflict.partition_index = partition_index;
            conflict.volume_name = volume_name;
            continue;
        }
        auto &allocation = directory_allocations[{partition_index, volume_name}];
        allocation.first += reserved->payload_clusters;
        allocation.second += reserved->continuation_clusters;
    }

    for (auto &object : plan.objects) {
        if (!std::ranges::contains(object.actions, PackageImportObjectAction::insert) ||
            std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            continue;
        }
        const auto *package_node = node_by_id(packages[object.package_index], object.node_id);
        const auto clusters =
            std::max<std::uint32_t>(2U, static_cast<std::uint32_t>((package_node->payload_size_bytes + 1023U) / 1024U));
        auto &capacity = capacities.at(object.partition_index);
        const auto reserved = reserve_clusters(capacity, clusters);
        if (!reserved) {
            mark_conflict(object);
            add_conflict(plan, "SFS_CLUSTER_EXHAUSTED",
                         "partition has insufficient clusters for the planned object payload");
            auto &conflict = plan.conflicts.back();
            conflict.package_index = object.package_index;
            conflict.root_index = object.root_index;
            conflict.package_id = object.package_id;
            conflict.node_id = object.node_id;
            conflict.partition_index = object.partition_index;
            conflict.volume_name = object.volume_name;
            continue;
        }
        object.payload_clusters = reserved->payload_clusters;
        object.continuation_clusters = reserved->continuation_clusters;
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
        object.target_wave_data_reference_value = canonical->second->target_wave_data_reference_value;
        if (std::ranges::contains(canonical->second->actions, PackageImportObjectAction::conflict))
            mark_conflict(object);
    }

    struct SbnkTargetMetadata {
        std::set<std::uint8_t> program_numbers;
        bool sample_bank_member{};
    };
    using PhysicalObjectKey = std::pair<std::uint8_t, std::uint32_t>;
    std::map<PhysicalObjectKey, SbnkTargetMetadata> sbnk_metadata;
    for (const auto &object : plan.objects) {
        if (object.object_type != "SBNK" || !object.target_sfs_id)
            continue;
        auto &metadata = sbnk_metadata[{object.partition_index, *object.target_sfs_id}];
        if (!object.existing_object_key)
            continue;
        const auto found = existing_by_key.find(*object.existing_object_key);
        if (found == existing_by_key.end())
            continue;
        if (const auto *sample = std::get_if<CurrentSbnk>(&existing[found->second].snapshot->object.payload)) {
            metadata.program_numbers.insert(sample->linked_program_numbers.begin(),
                                            sample->linked_program_numbers.end());
            metadata.sample_bank_member = (sample->sample_flags & 1U) != 0U;
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
                metadata.sample_bank_member = true;
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
        object.target_sample_bank_member = metadata->second.sample_bank_member;
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
            const auto found = existing_by_key.find(*object.existing_object_key);
            if (found == existing_by_key.end())
                return std::unexpected{planner_error("planned existing package object is missing")};
            auto &existing_object = existing[found->second];
            if (auto loaded = ensure_existing_identity(
                    existing_object, container, retained_session ? retained_session->stats : nullptr, cancellation);
                !loaded)
                return std::unexpected{loaded.error()};
            if (std::ranges::equal(*relocated, existing_payload(existing_object)))
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
        delta.infrastructure_clusters += destination.infrastructure_clusters;
    }
    for (const auto &[key, growth] : directory_allocations) {
        auto &delta = allocation[key];
        delta.partition_index = key.first;
        delta.volume_name = key.second;
        delta.directory_growth_clusters += growth.first;
        delta.directory_continuation_clusters += growth.second;
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
        if (std::ranges::contains(object.actions, PackageImportObjectAction::conflict))
            ++delta.blocked_object_count;
    }
    for (auto &[key, delta] : allocation) {
        const auto &capacity = capacities.at(key.first);
        delta.remaining_object_ids = capacity.free_ids.size() - capacity.next_id;
        delta.remaining_clusters = remaining_clusters(capacity);
        delta.additional_allocated_bytes =
            (delta.payload_clusters + delta.continuation_clusters + delta.directory_growth_clusters +
             delta.directory_continuation_clusters + delta.infrastructure_clusters) *
            1024U;
        plan.allocation.push_back(std::move(delta));
    }

    if (revalidate_target) {
        if (!before)
            return std::unexpected{planner_error("target snapshot is unavailable for revalidation")};
        const auto after = package_internal::sha256_reader(*target_reader, cancellation);
        if (!after)
            return std::unexpected{after.error()};
        if (*after != *before)
            return std::unexpected{stale_plan_error("target image changed while its import plan was built")};
    }

    std::ranges::sort(plan.conflicts, [](const auto &left, const auto &right) {
        return std::tie(left.code, left.package_index, left.root_index, left.package_id, left.node_id,
                        left.partition_index, left.group_name, left.volume_name, left.message) <
               std::tie(right.code, right.package_index, right.root_index, right.package_id, right.node_id,
                        right.partition_index, right.group_name, right.volume_name, right.message);
    });
    plan.plan_id = package_import_internal::plan_identity(plan);
    if (const auto verified = verify_package_import_plan(plan); !verified)
        return std::unexpected{verified.error()};
    return plan;
}

} // namespace axk::package_import_internal
