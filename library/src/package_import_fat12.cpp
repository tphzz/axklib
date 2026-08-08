#include "package_import_support.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/writer_internal.hpp"

namespace axk::package_import_internal {

Result<PackageImportPlan> plan_fat12_import(const RandomAccessReader &target_reader,
                                            std::span<const PortablePackage> packages,
                                            const PackageImportRequest &request, const MediaContainer &target,
                                            PackageImportPlan plan, const package_internal::Sha256Digest &before,
                                            bool revalidate_target, const CancellationToken &cancellation) {
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
            candidates.push_back({&package, node, destination, name, std::move(*normalized), {}, {}});
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
    if (auto adjusted = plan_program_assignment_adjustments(candidates, existing, plan); !adjusted)
        return std::unexpected{adjusted.error()};
    std::set<std::uint32_t> used_wave_data_reference_values;
    for (const auto &item : existing) {
        if (item.wave_data_reference_value)
            used_wave_data_reference_values.insert(*item.wave_data_reference_value);
    }
    std::uint32_t next_wave_data_reference_value = 0x016b1dbcU;
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
            const auto adjusted_program_reuse =
                candidate.node->object_type == "PROG" && !candidate.cleared_program_assignment_ordinals.empty() &&
                matches.front()->normalized_sha256 == candidate.unadjusted_normalized_sha256;
            if (matches.front()->normalized_sha256 == object.normalized_sha256 || adjusted_program_reuse) {
                object.actions.push_back(PackageImportObjectAction::reuse);
                object.existing_object_key = matches.front()->snapshot->key;
                object.target_wave_data_reference_value = matches.front()->wave_data_reference_value;
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
                    object.target_wave_data_reference_value = canonical.target_wave_data_reference_value;
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
                    while (used_wave_data_reference_values.contains(next_wave_data_reference_value))
                        next_wave_data_reference_value += 0x100U;
                    object.target_wave_data_reference_value = next_wave_data_reference_value;
                    used_wave_data_reference_values.insert(next_wave_data_reference_value);
                    next_wave_data_reference_value += 0x100U;
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
        object.target_wave_data_reference_value = canonical->second->target_wave_data_reference_value;
        if (std::ranges::contains(canonical->second->actions, PackageImportObjectAction::conflict))
            mark_conflict(object);
    }

    struct BankMetadata {
        std::set<std::uint8_t> programs;
        bool sample_bank_member{};
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
            if (const auto *sample = std::get_if<CurrentSbnk>(&found->snapshot->object.payload)) {
                metadata.programs.insert(sample->linked_program_numbers.begin(), sample->linked_program_numbers.end());
                metadata.sample_bank_member = (sample->sample_flags & 1U) != 0U;
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
                metadata.sample_bank_member = true;
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
        object.target_sample_bank_member = metadata->second.sample_bank_member;
    }

    if (plan.conflicts.empty()) {
        for (auto &object : plan.objects) {
            const auto *node = node_by_id(packages[object.package_index], object.node_id);
            if (node == nullptr)
                return std::unexpected{planner_error("planned FAT12 package node is missing")};
            if (object.object_type == "SMPL" && object.existing_object_key &&
                !object.target_wave_data_reference_value) {
                continue;
            }
            auto context = relocation_context(packages[object.package_index], plan, object);
            if (!context)
                return std::unexpected{context.error()};
            auto relocated = package_internal::relocate_package_node(packages[object.package_index], *node, *context);
            if (!relocated)
                return std::unexpected{relocated.error()};
            auto decoded = package_internal::decode_package_object(*relocated);
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
                if (object.object_type != "SBNK" && object.object_type != "PROG") {
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
        if (!media_object_paths.contains(file.path) && !detail::is_yamaha_floppy_catalog_path(file.path))
            ++retained_files;
    }
    std::uint64_t inserted_objects{};
    for (const auto &object : plan.objects) {
        if (std::ranges::contains(object.actions, PackageImportObjectAction::insert) &&
            !std::ranges::contains(object.actions, PackageImportObjectAction::conflict)) {
            ++inserted_objects;
        }
    }
    const auto final_entries = catalog->objects.size() + retained_files + inserted_objects + 1U;
    if (final_entries > fat.geometry().root_entry_count)
        add_conflict(plan, "FAT12_ROOT_ENTRY_EXHAUSTED", "FAT12 root directory cannot contain the planned import");
    const auto cluster_size = fat.geometry().cluster_size();
    std::uint64_t used_clusters = (detail::yamaha_floppy_catalog_bytes + cluster_size - 1U) / cluster_size;
    for (const auto &file : fat.files()) {
        if (!detail::is_yamaha_floppy_catalog_path(file.path))
            used_clusters += (file.size + cluster_size - 1U) / cluster_size;
    }
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
    for (const auto &object : plan.objects) {
        const auto conflict = std::ranges::contains(object.actions, PackageImportObjectAction::conflict);
        if (conflict)
            ++delta.blocked_object_count;
        if (!conflict && std::ranges::contains(object.actions, PackageImportObjectAction::insert))
            ++delta.inserted_object_count;
        if (!conflict && std::ranges::contains(object.actions, PackageImportObjectAction::reuse))
            ++delta.reused_object_count;
        delta.payload_clusters += object.payload_clusters;
    }
    delta.directory_growth_bytes = inserted_objects * 32U;
    delta.additional_allocated_bytes = delta.payload_clusters * cluster_size;
    delta.remaining_object_ids =
        final_entries > fat.geometry().root_entry_count ? 0U : fat.geometry().root_entry_count - final_entries;
    delta.remaining_clusters =
        used_clusters > fat.geometry().data_cluster_count ? 0U : fat.geometry().data_cluster_count - used_clusters;
    plan.allocation.push_back(std::move(delta));

    if (revalidate_target) {
        const auto after = package_internal::sha256_reader(target_reader, cancellation);
        if (!after)
            return std::unexpected{after.error()};
        if (*after != before)
            return std::unexpected{stale_plan_error("target image changed while its import plan was built")};
    }
    std::ranges::sort(plan.conflicts, [](const auto &left, const auto &right) {
        return std::tie(left.code, left.package_index, left.root_index, left.package_id, left.node_id,
                        left.partition_index, left.group_name, left.volume_name, left.raw_group, left.raw_volume,
                        left.message) < std::tie(right.code, right.package_index, right.root_index, right.package_id,
                                                 right.node_id, right.partition_index, right.group_name,
                                                 right.volume_name, right.raw_group, right.raw_volume, right.message);
    });
    plan.plan_id = package_import_internal::plan_identity(plan);
    return plan;
}

} // namespace axk::package_import_internal
