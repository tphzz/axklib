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
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

bool has_action(const PlannedPackageObject &object, PackageImportObjectAction action) {
    return std::ranges::contains(object.actions, action);
}

const PackageNode *package_node(const PortablePackage &package, std::string_view node_id) {
    const auto found = std::ranges::find(package.nodes, node_id, &PackageNode::node_id);
    return found == package.nodes.end() ? nullptr : &*found;
}

const PlannedPackageObject *planned_node(const PackageImportPlan &plan, const PlannedPackageObject &owner,
                                         std::string_view node_id) {
    const auto found = std::ranges::find_if(plan.objects, [&](const auto &candidate) {
        return candidate.package_index == owner.package_index && candidate.root_index == owner.root_index &&
               candidate.node_id == node_id && candidate.partition_index == owner.partition_index &&
               candidate.group_name == owner.group_name && candidate.volume_name == owner.volume_name &&
               candidate.raw_group == owner.raw_group && candidate.raw_volume == owner.raw_volume;
    });
    return found == plan.objects.end() ? nullptr : &*found;
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
            return std::unexpected{transaction_error("package import plan omits a relationship target action")};
        }
        context.edge_target_names.emplace(edge.edge_id, target->destination_name);
        if (target->target_wave_data_reference_value)
            context.edge_target_reference_values.emplace(edge.edge_id, *target->target_wave_data_reference_value);
    }
    return context;
}

Result<std::string> normalized_payload_digest(std::span<const std::byte> payload) {
    auto decoded = package_internal::decode_package_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    auto profile = package_internal::build_relocation_profile(*decoded, payload);
    if (!profile)
        return std::unexpected{profile.error()};
    return package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
}

std::optional<std::pair<std::string, std::string>> package_iso_raw_scope(const ObjectSnapshot &snapshot) {
    if (!snapshot.placement)
        return std::nullopt;
    const auto &path = snapshot.placement->container_directory;
    const auto separator = path.find('/');
    if (separator == std::string::npos || path.find('/', separator + 1U) != std::string::npos)
        return std::nullopt;
    return std::pair{path.substr(0, separator), path.substr(separator + 1U)};
}

template <typename Replace>
Result<void> apply_existing_flat_program_assignment_adjustments(std::span<const MediaObject> objects,
                                                                const PackageImportPlan &plan, Replace replace,
                                                                const CancellationToken &cancellation) {
    std::map<std::string, std::vector<const PackageProgramAssignmentAdjustment *>, std::less<>> grouped;
    for (const auto &adjustment : plan.program_assignment_adjustments) {
        if (adjustment.origin == PackageProgramAssignmentOrigin::existing_program)
            grouped[*adjustment.existing_object_key].push_back(&adjustment);
    }
    for (const auto &[object_key, adjustments] : grouped) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto object = std::ranges::find(objects, object_key, &MediaObject::key);
        if (object == objects.end())
            return std::unexpected{transaction_error("planned existing Program adjustment owner is unavailable")};
        auto cleared = clear_program_assignment_adjustments(object->raw_payload, adjustments);
        if (!cleared)
            return std::unexpected{cleared.error()};
        if (auto replaced = replace(*object, std::move(*cleared)); !replaced)
            return std::unexpected{replaced.error()};
    }
    return {};
}

Result<void> validate_flat_program_assignment_adjustments(
    const ObjectCatalog &catalog, const PackageImportPlan &plan,
    const std::map<std::string, const ObjectSnapshot *, std::less<>> &actual_by_action) {
    std::map<std::string, std::vector<const PackageProgramAssignmentAdjustment *>, std::less<>> grouped;
    for (const auto &adjustment : plan.program_assignment_adjustments) {
        const auto owner = adjustment.origin == PackageProgramAssignmentOrigin::imported_program
                               ? "action:" + *adjustment.action_id
                               : "existing:" + *adjustment.existing_object_key;
        grouped[owner].push_back(&adjustment);
    }
    for (const auto &[owner, adjustments] : grouped) {
        const ObjectSnapshot *snapshot{};
        if (owner.starts_with("action:")) {
            const auto found = actual_by_action.find(owner.substr(7U));
            if (found != actual_by_action.end())
                snapshot = found->second;
        } else {
            std::vector<const ObjectSnapshot *> matches;
            const auto &expected = *adjustments.front();
            for (const auto &candidate : catalog.objects) {
                const auto scope = package_iso_raw_scope(candidate);
                const auto scope_matches =
                    plan.target_kind != MediaKind::iso9660 ||
                    (scope && scope->first == expected.raw_group && scope->second == expected.raw_volume);
                if (candidate.object.header.raw_type == "PROG" &&
                    candidate.object.header.name == expected.program_slot && scope_matches) {
                    matches.push_back(&candidate);
                }
            }
            if (matches.size() == 1U)
                snapshot = matches.front();
        }
        if (snapshot == nullptr)
            return std::unexpected{transaction_error("adjusted Program is not unique in the rebuilt media")};
        if (auto validated = validate_cleared_program_assignment_adjustments(*snapshot, adjustments); !validated)
            return std::unexpected{validated.error()};
    }
    return {};
}

Result<void> validate_flat_package_result(const std::filesystem::path &temporary,
                                          std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                                          const CancellationToken &cancellation) {
    auto media = open_media(temporary, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != plan.target_kind)
        return std::unexpected{transaction_error("package result reopened as the wrong media kind")};
    auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    const auto graph = build_relationship_graph(*catalog);
    std::map<std::string, const ObjectSnapshot *, std::less<>> actual_by_action;
    for (const auto &object : plan.objects) {
        std::vector<const ObjectSnapshot *> matches;
        for (const auto &snapshot : catalog->objects) {
            const auto scope = package_iso_raw_scope(snapshot);
            const auto scope_matches =
                plan.target_kind != MediaKind::iso9660 ||
                (scope && scope->first == object.raw_group && scope->second == object.raw_volume);
            if (snapshot.object.header.raw_type == object.object_type &&
                snapshot.object.header.name == object.destination_name && scope_matches) {
                matches.push_back(&snapshot);
            }
        }
        if (matches.size() != 1U) {
            return std::unexpected{transaction_error("post-write package object is not unique in its media scope")};
        }
        auto normalized = normalized_payload_digest(matches.front()->raw_payload);
        if (!normalized)
            return std::unexpected{normalized.error()};
        if (*normalized != object.normalized_sha256)
            return std::unexpected{transaction_error("post-write package object changed identity")};
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
    if (auto adjusted = validate_flat_program_assignment_adjustments(*catalog, plan, actual_by_action); !adjusted)
        return std::unexpected{adjusted.error()};
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
            if (std::ranges::find_if(relationships, [&](const Relationship *actual) {
                    return actual->type == edge.role && actual->quality == RelationshipQuality::known &&
                           actual->target_key && *actual->target_key == target->second->key;
                }) == relationships.end()) {
                return std::unexpected{
                    transaction_error(std::format("post-write {} relationship from {} to {} "
                                                  "differs from the package plan",
                                                  edge.role, owner.destination_name, target_plan->destination_name))};
            }
        }
    }
    return {};
}

Result<PackageImportReport> apply_fat12_package_import(const std::filesystem::path &target_path,
                                                       std::span<const PortablePackage> packages,
                                                       const PackageImportPlan &plan,
                                                       const std::filesystem::path &output_path, bool overwrite,
                                                       const CancellationToken &cancellation, ProgressSink *progress) {
    auto media = open_media(target_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::fat12_floppy)
        return std::unexpected{transaction_error("FAT12 package target changed media kind")};
    auto media_objects = media->objects(64U * 1024U * 1024U, cancellation);
    if (!media_objects)
        return std::unexpected{media_objects.error()};

    detail::PreparedMediaImage prepared;
    prepared.manifest.schema_version = build_manifest_schema_version;
    prepared.manifest.format = MediaImageFormat::fat12_floppy;
    std::map<std::string, std::size_t, std::less<>> object_indices;
    std::set<std::string, std::less<>> object_paths;
    for (const auto &object : *media_objects) {
        object_indices.emplace(object.key, prepared.objects.size());
        object_paths.insert(object.logical_path);
        prepared.objects.push_back({object.decoded.header.type, object.decoded.header.name, object.raw_payload});
    }
    const auto &fat = std::get<FatImage>(media->storage());
    for (const auto &file : fat.files()) {
        if (object_paths.contains(file.path))
            continue;
        auto payload = fat.read_file(file, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        prepared.retained_files.push_back({file.path, std::move(*payload)});
    }
    if (auto adjusted = apply_existing_flat_program_assignment_adjustments(
            *media_objects, plan,
            [&](const MediaObject &object, std::vector<std::byte> payload) -> Result<void> {
                const auto existing = object_indices.find(object.key);
                if (existing == object_indices.end())
                    return std::unexpected{transaction_error("planned FAT12 Program adjustment owner is absent")};
                prepared.objects[existing->second] = {object.decoded.header.type, object.decoded.header.name,
                                                      std::move(payload)};
                return {};
            },
            cancellation);
        !adjusted) {
        return std::unexpected{adjusted.error()};
    }

    std::set<std::string, std::less<>> updated_physical_objects;
    std::uint64_t completed{};
    for (const auto &object : plan.objects) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        if (!object.canonical_action_id) {
            const auto physical_key =
                object.existing_object_key ? "existing:" + *object.existing_object_key : "planned:" + object.action_id;
            if (updated_physical_objects.insert(physical_key).second &&
                (std::ranges::contains(object.actions, PackageImportObjectAction::insert) ||
                 std::ranges::contains(object.actions, PackageImportObjectAction::relocate))) {
                const auto &package = packages[object.package_index];
                const auto *node = package_node(package, object.node_id);
                if (node == nullptr)
                    return std::unexpected{transaction_error("FAT12 package node is missing")};
                auto context = relocation_context(package, plan, object);
                if (!context)
                    return std::unexpected{context.error()};
                auto payload = package_internal::relocate_package_node(package, *node, *context);
                if (!payload)
                    return std::unexpected{payload.error()};
                auto decoded = package_internal::decode_package_object(*payload);
                if (!decoded)
                    return std::unexpected{decoded.error()};
                if (object.existing_object_key) {
                    const auto existing = object_indices.find(*object.existing_object_key);
                    if (existing == object_indices.end())
                        return std::unexpected{transaction_error("planned FAT12 reused object is absent")};
                    prepared.objects[existing->second] = {decoded->header.type, object.destination_name,
                                                          std::move(*payload)};
                } else {
                    prepared.objects.push_back({decoded->header.type, object.destination_name, std::move(*payload)});
                }
            }
        }
        ++completed;
        if (progress) {
            progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                              "rebuilding FAT12 portable package graph", output_path.string()});
        }
    }
    std::ranges::sort(prepared.objects, [](const auto &left, const auto &right) {
        return std::tuple{left.type, left.name, left.size()} < std::tuple{right.type, right.name, right.size()};
    });
    std::ranges::sort(prepared.retained_files, {}, &detail::PreparedMediaFile::path);
    const auto validator = [&](const std::filesystem::path &temporary) {
        return validate_flat_package_result(temporary, packages, plan, cancellation);
    };
    auto written = detail::write_prepared_media_image(prepared, output_path, overwrite, cancellation, validator);
    if (!written)
        return std::unexpected{written.error()};
    auto reader = FileReader::open(output_path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected{digest.error()};
    return PackageImportReport{target_path,
                               output_path,
                               plan.plan_id,
                               plan.target_snapshot_id,
                               package_internal::hex_digest(*digest),
                               true,
                               plan.objects,
                               plan.program_assignment_adjustments,
                               plan.allocation,
                               std::move(written->publication)};
}

Result<PackageImportReport>
apply_iso9660_package_import(const std::filesystem::path &target_path, std::span<const PortablePackage> packages,
                             const PackageImportPlan &plan, const std::filesystem::path &output_path, bool overwrite,
                             const CancellationToken &cancellation, ProgressSink *progress) {
    auto media = open_media(target_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::iso9660)
        return std::unexpected{transaction_error("ISO9660 package target changed media kind")};
    auto media_objects = media->objects(64U * 1024U * 1024U, cancellation);
    if (!media_objects)
        return std::unexpected{media_objects.error()};
    const auto &iso = std::get<IsoImage>(media->storage());

    detail::PreparedMediaImage prepared;
    prepared.manifest.schema_version = build_manifest_schema_version;
    prepared.manifest.format = MediaImageFormat::iso9660;
    prepared.manifest.iso_volume_id = iso.volume_id();

    std::map<std::string, std::string, std::less<>> group_labels;
    for (const auto &[raw, label] : iso.group_labels())
        group_labels.emplace(raw, label);
    std::map<std::pair<std::string, std::string>, std::string> volume_labels;
    for (const auto &[raw_path, label] : iso.volume_labels()) {
        const auto separator = raw_path.find('/');
        if (separator != std::string::npos)
            volume_labels.emplace(std::pair{raw_path.substr(0, separator), raw_path.substr(separator + 1U)}, label);
    }
    std::map<std::pair<std::string, std::string>, std::size_t> volume_indices;
    std::map<std::string, std::size_t, std::less<>> existing_group_volume_counts;
    for (const auto &file : iso.files()) {
        if (!file.is_directory)
            continue;
        const auto separator = file.path.find('/');
        if (separator == std::string::npos || file.path.find('/', separator + 1U) != std::string::npos)
            continue;
        const auto scope = std::pair{file.path.substr(0, separator), file.path.substr(separator + 1U)};
        const auto group = group_labels.find(scope.first);
        const auto volume = volume_labels.find(scope);
        if (group == group_labels.end() || volume == volume_labels.end())
            return std::unexpected{transaction_error("cataloged ISO9660 volume labels changed after planning")};
        volume_indices.emplace(scope, prepared.iso_volumes.size());
        ++existing_group_volume_counts[scope.first];
        prepared.iso_volumes.push_back({scope.first, group->second, scope.second, volume->second, {}});
    }
    for (const auto &destination : plan.destinations) {
        const auto scope = std::pair{destination.raw_group, destination.raw_volume};
        if (volume_indices.contains(scope))
            continue;
        if (!destination.create)
            return std::unexpected{transaction_error("planned ISO9660 destination is absent")};
        volume_indices.emplace(scope, prepared.iso_volumes.size());
        prepared.iso_volumes.push_back(
            {destination.raw_group, destination.group_name, destination.raw_volume, destination.volume_name, {}});
    }
    std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> object_indices;
    std::set<std::string, std::less<>> generated_files;
    std::map<std::string, std::size_t, std::less<>> group_volume_counts;
    for (const auto &volume : prepared.iso_volumes)
        ++group_volume_counts[volume.raw_group];
    for (const auto &[group, count] : group_volume_counts) {
        generated_files.insert(group + "/0000");
        generated_files.insert(group + "/" + std::format("F{:03}", count + 1U));
    }
    for (const auto &[group, count] : existing_group_volume_counts)
        generated_files.insert(group + "/" + std::format("F{:03}", count + 1U));
    for (const auto &object : *media_objects) {
        const auto scope = std::pair{object.raw_group, object.raw_volume};
        const auto volume = volume_indices.find(scope);
        if (volume == volume_indices.end())
            return std::unexpected{transaction_error("existing ISO9660 object has no cataloged raw volume")};
        auto &objects = prepared.iso_volumes[volume->second].objects;
        object_indices.emplace(object.key, std::pair{volume->second, objects.size()});
        objects.push_back({object.decoded.header.type, object.decoded.header.name, object.raw_payload});
        generated_files.insert(object.logical_path);
        const auto separator = object.logical_path.rfind('/');
        if (separator != std::string::npos)
            generated_files.insert(object.logical_path.substr(0, separator) + "/0000");
    }
    for (const auto &file : iso.files()) {
        if (file.is_directory || generated_files.contains(file.path))
            continue;
        auto payload = iso.read_file(file, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        prepared.retained_files.push_back({file.path, std::move(*payload)});
    }
    if (auto adjusted = apply_existing_flat_program_assignment_adjustments(
            *media_objects, plan,
            [&](const MediaObject &object, std::vector<std::byte> payload) -> Result<void> {
                const auto existing = object_indices.find(object.key);
                if (existing == object_indices.end())
                    return std::unexpected{transaction_error("planned ISO9660 Program adjustment owner is absent")};
                const auto [volume_index, object_index] = existing->second;
                prepared.iso_volumes[volume_index].objects[object_index] = {
                    object.decoded.header.type, object.decoded.header.name, std::move(payload)};
                return {};
            },
            cancellation);
        !adjusted) {
        return std::unexpected{adjusted.error()};
    }

    std::set<std::string, std::less<>> updated_physical_objects;
    std::uint64_t completed{};
    for (const auto &object : plan.objects) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        if (!object.canonical_action_id) {
            const auto physical_key =
                object.existing_object_key ? "existing:" + *object.existing_object_key : "planned:" + object.action_id;
            if (updated_physical_objects.insert(physical_key).second &&
                (has_action(object, PackageImportObjectAction::insert) ||
                 has_action(object, PackageImportObjectAction::relocate))) {
                const auto &package = packages[object.package_index];
                const auto *node = package_node(package, object.node_id);
                if (node == nullptr)
                    return std::unexpected{transaction_error("ISO9660 package node is missing")};
                auto context = relocation_context(package, plan, object);
                if (!context)
                    return std::unexpected{context.error()};
                auto payload = package_internal::relocate_package_node(package, *node, *context);
                if (!payload)
                    return std::unexpected{payload.error()};
                auto decoded = package_internal::decode_package_object(*payload);
                if (!decoded)
                    return std::unexpected{decoded.error()};
                if (object.existing_object_key) {
                    const auto existing = object_indices.find(*object.existing_object_key);
                    if (existing == object_indices.end())
                        return std::unexpected{transaction_error("planned ISO9660 reused object is absent")};
                    auto &[volume_index, object_index] = existing->second;
                    prepared.iso_volumes[volume_index].objects[object_index] = {
                        decoded->header.type, object.destination_name, std::move(*payload)};
                } else {
                    const auto volume = volume_indices.find({object.raw_group, object.raw_volume});
                    if (volume == volume_indices.end())
                        return std::unexpected{transaction_error("planned ISO9660 insertion volume is absent")};
                    prepared.iso_volumes[volume->second].objects.push_back(
                        {decoded->header.type, object.destination_name, std::move(*payload)});
                }
            }
        }
        ++completed;
        if (progress) {
            progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                              "rebuilding ISO9660 portable package graph", output_path.string()});
        }
    }
    std::ranges::sort(prepared.iso_volumes, [](const auto &left, const auto &right) {
        return std::tie(left.raw_group, left.raw_volume) < std::tie(right.raw_group, right.raw_volume);
    });
    for (auto &volume : prepared.iso_volumes) {
        std::ranges::sort(volume.objects, [](const auto &left, const auto &right) {
            return std::tuple{left.type, left.name, left.size()} < std::tuple{right.type, right.name, right.size()};
        });
    }
    std::ranges::sort(prepared.retained_files, {}, &detail::PreparedMediaFile::path);
    const auto validator = [&](const std::filesystem::path &temporary) -> Result<void> {
        if (plan.allocation.empty())
            return std::unexpected{transaction_error("ISO9660 package plan has no projected allocation")};
        std::error_code size_error;
        const auto actual_size = std::filesystem::file_size(temporary, size_error);
        const auto expected_size = plan.allocation.front().projected_image_size_bytes;
        if (size_error || actual_size != expected_size ||
            std::ranges::any_of(plan.allocation, [&](const auto &allocation) {
                return allocation.projected_image_size_bytes != expected_size;
            })) {
            return std::unexpected{transaction_error(
                size_error ? std::format("cannot inspect rebuilt ISO9660 size: {}", size_error.message())
                           : std::format("rebuilt ISO9660 size differs from the "
                                         "package plan: planned "
                                         "{} bytes, emitted {} bytes",
                                         expected_size, actual_size))};
        }
        return validate_flat_package_result(temporary, packages, plan, cancellation);
    };
    auto written = detail::write_prepared_media_image(prepared, output_path, overwrite, cancellation, validator);
    if (!written)
        return std::unexpected{written.error()};
    auto reader = FileReader::open(output_path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected{digest.error()};
    return PackageImportReport{target_path,
                               output_path,
                               plan.plan_id,
                               plan.target_snapshot_id,
                               package_internal::hex_digest(*digest),
                               true,
                               plan.objects,
                               plan.program_assignment_adjustments,
                               plan.allocation,
                               std::move(written->publication)};
}

} // namespace axk::alteration_internal
