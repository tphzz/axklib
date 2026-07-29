#include "package_import_support.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

namespace axk::package_import_internal {

Result<PackageImportPlan> plan_iso9660_import(const RandomAccessReader &target_reader,
                                              std::span<const PortablePackage> packages,
                                              const PackageImportRequest &request, const MediaContainer &target,
                                              PackageImportPlan plan, const package_internal::Sha256Digest &before,
                                              bool revalidate_target, const CancellationToken &cancellation) {
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
    std::map<IsoScopeKey, std::set<std::uint32_t>> used_wave_data_reference_values;
    for (const auto &item : existing) {
        const auto scope = iso_raw_scope(*item.snapshot);
        if (scope && item.wave_data_reference_value)
            used_wave_data_reference_values[*scope].insert(*item.wave_data_reference_value);
    }
    std::map<IsoScopeKey, std::uint32_t> next_wave_data_reference_values;
    for (const auto &[scope, ignored] : planned_scopes) {
        (void)ignored;
        next_wave_data_reference_values.emplace(scope, 0x016b1dbcU);
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
                object.target_wave_data_reference_value = matches.front()->wave_data_reference_value;
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
                    object.target_wave_data_reference_value = canonical.target_wave_data_reference_value;
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
                    auto &next = next_wave_data_reference_values.at(scope);
                    while (used_wave_data_reference_values[scope].contains(next))
                        next += 0x100U;
                    object.target_wave_data_reference_value = next;
                    used_wave_data_reference_values[scope].insert(next);
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
        object.target_wave_data_reference_value = canonical->second->target_wave_data_reference_value;
        if (std::ranges::contains(canonical->second->actions, PackageImportObjectAction::conflict))
            mark_conflict(object);
    }

    struct BankMetadata {
        std::set<std::uint8_t> programs;
        bool sample_bank_member{};
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
                (edge.role != "SBAC_SLOT_TO_SBNK" && edge.role != "PROG_ASSIGNMENT_TO_SBNK"))
                continue;
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
                return std::unexpected{planner_error("planned ISO9660 package node is missing")};
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
        if (std::ranges::contains(object.actions, PackageImportObjectAction::conflict))
            ++delta.blocked_object_count;
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
        delta.additional_allocated_bytes = delta.payload_sectors * 2048U;
        plan.allocation.push_back(std::move(delta));
    }

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
    if (const auto verified = verify_package_import_plan(plan); !verified)
        return std::unexpected{verified.error()};
    return plan;
}

} // namespace axk::package_import_internal
