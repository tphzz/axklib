#include "axklib/package.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/package_relocation.hpp"
#include "axklib/relationship.hpp"
#include "axklib/utf8.hpp"

#include "package_internal.hpp"
#include "package_manifest_internal.hpp"
#include "relationship_policy.hpp"

namespace axk {

bool package_internal::portable_inactive_program_relationship(const Relationship &relationship) {
    return (relationship.assignment_state == AssignmentState::source_load ||
            relationship.assignment_state == AssignmentState::visible_off) &&
           relationship.quality == RelationshipQuality::known && relationship.target_key.has_value();
}

namespace {

using Json = nlohmann::json;
using package_internal::closure_relationship;
using package_internal::derive_kind;
using package_internal::digest_text;
using package_internal::edge_id;
using package_internal::media_kind_name;
using package_internal::object_format_name;
using package_internal::object_type_name;
using package_internal::package_error;
using package_internal::root_object_type;
using package_internal::schema_version;
using package_internal::string_bytes;
using package_internal::waveform_digests;

struct ProvisionalNode {
    const ObjectSnapshot *snapshot{};
    package_internal::RelocationProfile profile;
    std::string payload_digest;
    std::string normalized_digest;
    std::string identity_digest;
    std::optional<std::string> semantic_digest;
    std::optional<std::string> audio_digest;
};

struct SelectedRoot {
    PackageRootKind kind{PackageRootKind::volume};
    std::string display_name;
    std::vector<const ObjectSnapshot *> seeds;
};

Error root_error(const PackageRootSelector &selector, std::string message) {
    ErrorContext context;
    context.partition_index = selector.partition_index;
    if (!selector.volume_name.empty())
        context.volume_name = selector.volume_name;
    if (selector.kind != PackageRootKind::volume)
        context.object_name = selector.object_name;
    return make_error(ErrorCode::object_missing, ErrorCategory::manifest, std::move(message), std::move(context));
}

bool matches_scope(const ObjectSnapshot &object, const PackageRootSelector &selector) {
    if (!object.placement) {
        return selector.kind != PackageRootKind::volume && !selector.partition_index && selector.group_name.empty() &&
               !selector.volume_directory_id && selector.volume_name.empty();
    }
    if (selector.partition_index && object.placement->partition.value != *selector.partition_index)
        return false;
    if (selector.volume_directory_id && object.placement->volume_directory.value != *selector.volume_directory_id) {
        return false;
    }
    if (!selector.group_name.empty() && object.placement->partition_name != selector.group_name)
        return false;
    return selector.volume_name.empty() || object.placement->volume_name == selector.volume_name;
}

Result<std::vector<SelectedRoot>> select_roots(const ObjectCatalog &catalog,
                                               std::span<const PackageRootSelector> selectors) {
    if (selectors.empty())
        return std::unexpected{
            package_error("at least one package root selector is required", ErrorCode::invalid_argument)};
    std::vector<SelectedRoot> result;
    result.reserve(selectors.size());
    for (const auto &selector : selectors) {
        SelectedRoot root;
        root.kind = selector.kind;
        if (selector.kind == PackageRootKind::volume) {
            for (const auto &object : catalog.objects) {
                if (matches_scope(object, selector))
                    root.seeds.push_back(&object);
            }
            if (root.seeds.empty())
                return std::unexpected{root_error(selector, "package volume selector matches no objects")};
            const auto &placement = *root.seeds.front()->placement;
            root.display_name = placement.volume_name;
            for (const auto *object : root.seeds) {
                if (object->placement->partition.value != placement.partition.value ||
                    object->placement->volume_directory.value != placement.volume_directory.value ||
                    object->placement->container_directory != placement.container_directory) {
                    return std::unexpected{root_error(selector, "package volume selector matches "
                                                                "more than one volume")};
                }
            }
        } else {
            const auto expected = root_object_type(selector.kind);
            for (const auto &object : catalog.objects) {
                if (object.object.header.type != expected ||
                    (!selector.object_name.empty() && object.object.header.name != selector.object_name) ||
                    !matches_scope(object, selector) || (selector.object_key && object.key != *selector.object_key)) {
                    continue;
                }
                root.seeds.push_back(&object);
            }
            if (root.seeds.empty())
                return std::unexpected{root_error(selector, "package object selector matches no object")};
            if (root.seeds.size() != 1U)
                return std::unexpected{root_error(selector, "package object selector is "
                                                            "ambiguous; provide an object key")};
            root.display_name = root.seeds.front()->object.header.name;
        }
        result.push_back(std::move(root));
    }
    return result;
}

bool portable_program_assignment(AssignmentState state) {
    return state == AssignmentState::active || state == AssignmentState::source_load ||
           state == AssignmentState::visible_off;
}

Result<std::vector<const Relationship *>>
required_relationships(const ObjectSnapshot &object, const RelationshipGraph &graph,
                       const std::map<std::string, const ObjectSnapshot *, std::less<>> &objects) {
    std::vector<const Relationship *> candidates;
    for (const auto *relationship : graph.children(object.key)) {
        if (!closure_relationship(relationship->type))
            continue;
        if (relationship->type.starts_with("PROG_ASSIGNMENT_TO_") &&
            !portable_program_assignment(relationship->assignment_state)) {
            continue;
        }
        candidates.push_back(relationship);
    }

    const auto require_one = [&](std::string_view role, std::optional<std::size_t> assignment_index =
                                                            std::nullopt) -> Result<const Relationship *> {
        std::vector<const Relationship *> matches;
        for (const auto *row : candidates) {
            if (row->type == role && (!assignment_index || row->assignment_index == assignment_index))
                matches.push_back(row);
        }
        if (matches.size() != 1U || matches.front()->quality != RelationshipQuality::known ||
            !matches.front()->target_key) {
            ErrorContext context;
            context.object_type = object.object.header.raw_type;
            context.object_name = object.object.header.name;
            return std::unexpected{make_error(
                matches.size() > 1U ? ErrorCode::relationship_ambiguous : ErrorCode::relationship_unresolved,
                ErrorCategory::relationship, std::format("package closure requires one known {} relationship", role),
                std::move(context))};
        }
        return matches.front();
    };

    std::vector<const Relationship *> result;
    if (const auto *sample = std::get_if<CurrentSbnk>(&object.object.payload)) {
        if (!sample->left.wave_data_name.empty()) {
            auto row = require_one("SBNK_LEFT_MEMBER_TO_SMPL");
            if (!row)
                return std::unexpected{row.error()};
            result.push_back(*row);
        }
        if (sample->right && !sample->right->wave_data_name.empty()) {
            auto row = require_one("SBNK_RIGHT_MEMBER_TO_SMPL");
            if (!row)
                return std::unexpected{row.error()};
            result.push_back(*row);
        }
    } else if (const auto *sample_bank = std::get_if<CurrentSbac>(&object.object.payload)) {
        const auto active_slots =
            std::ranges::count_if(sample_bank->slots, [](const SbacSlot &slot) { return !slot.name.empty(); });
        if (candidates.size() != static_cast<std::size_t>(active_slots)) {
            return std::unexpected{make_error(ErrorCode::relationship_unresolved, ErrorCategory::relationship,
                                              "Sample Bank package export cannot resolve every active "
                                              "Sample member")};
        }
        for (const auto *row : candidates) {
            if (row->type != "SBAC_SLOT_TO_SBNK" || row->quality != RelationshipQuality::known || !row->target_key) {
                return std::unexpected{make_error(ErrorCode::relationship_unresolved, ErrorCategory::relationship,
                                                  "Sample Bank package export cannot unambiguously identify "
                                                  "every Sample member in its source volume")};
            }
            result.push_back(row);
        }
    } else if (const auto *program = std::get_if<CurrentProg>(&object.object.payload)) {
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (assignment.name.empty() || std::to_integer<std::uint8_t>(assignment.raw_row[0x28U]) != 0xffU)
                continue;
            const auto role = assignment.kind == 0x11U   ? "PROG_ASSIGNMENT_TO_SBAC"
                              : assignment.kind == 0x10U ? "PROG_ASSIGNMENT_TO_SBNK"
                                                         : std::string_view{};
            if (role.empty())
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "active Program assignment has an unsupported target "
                                                  "kind")};
            const auto unresolved = std::ranges::find_if(candidates, [&](const Relationship *row) {
                return row->type == role && row->assignment_index == index &&
                       row->assignment_state == AssignmentState::active && row->quality != RelationshipQuality::known &&
                       !detail::relationship_has_exact_named_program_target(*row, objects);
            });
            if (unresolved != candidates.end())
                continue;
            auto row = require_one(role, index);
            if (!row)
                return std::unexpected{row.error()};
            result.push_back(*row);
        }
        for (const auto *row : candidates) {
            // Inactive diagnostic rows are portable only when their target is
            // exact. Ambiguous visible-off rows do not represent active Program
            // content.
            if (!package_internal::portable_inactive_program_relationship(*row))
                continue;
            if (std::ranges::find(result, row) == result.end())
                result.push_back(row);
        }
    }
    return result;
}

PackagePlacementHint placement_hint(const ObjectSnapshot &object) {
    if (!object.placement)
        return {};
    return {object.placement->partition_name, object.placement->volume_name, object.placement->category_name,
            object.placement->entry_name};
}

std::string identity_digest(const ObjectSnapshot &object, std::string_view normalized_digest) {
    std::string identity = object_type_name(object.object.header.type);
    identity.push_back('\0');
    identity += object.object.header.name;
    identity.push_back('\0');
    identity += normalized_digest;
    return digest_text(identity);
}

;

auto canonical_node_order(const ProvisionalNode &node) {
    const auto placement = placement_hint(*node.snapshot);
    return std::tuple{node.identity_digest, placement.group_name, placement.volume_name, placement.category_name,
                      placement.entry_name, node.payload_digest,  node.snapshot->key};
}

std::map<std::string, std::string, std::less<>> assign_node_ids(std::vector<ProvisionalNode> &nodes) {
    std::ranges::sort(nodes, {}, canonical_node_order);
    std::map<std::string, std::string, std::less<>> result;
    std::size_t begin{};
    while (begin < nodes.size()) {
        auto end = begin + 1U;
        while (end < nodes.size() && nodes[end].identity_digest == nodes[begin].identity_digest)
            ++end;
        for (auto index = begin; index < end; ++index) {
            auto digest = nodes[index].identity_digest;
            if (end - begin > 1U)
                digest = digest_text(std::format("{}#{}", digest, index - begin + 1U));
            result.emplace(nodes[index].snapshot->key, "n-" + digest);
        }
        begin = end;
    }
    return result;
}

Result<PackageBuild> build_selected_package(const MediaContainer &source, std::span<const SelectedRoot> selected,
                                            const RelationshipGraph &graph,
                                            const std::map<std::string, const ObjectSnapshot *, std::less<>> &objects,
                                            const CancellationToken &cancellation) {
    std::set<std::string, std::less<>> included_keys;
    std::vector<std::pair<const Relationship *, std::uint32_t>> included_relationships;
    std::vector<const ObjectSnapshot *> queue;
    for (const auto &root : selected)
        queue.insert(queue.end(), root.seeds.begin(), root.seeds.end());
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto *object = queue[cursor];
        if (!included_keys.emplace(object->key).second)
            continue;
        if (object->raw_payload.empty())
            return std::unexpected{package_error("package object has no retained raw payload")};
        const auto profile = package_internal::build_relocation_profile(object->object, object->raw_payload);
        if (!profile)
            return std::unexpected{profile.error()};
        auto required = required_relationships(*object, graph, objects);
        if (!required)
            return std::unexpected{required.error()};
        std::map<std::string, std::uint32_t, std::less<>> role_ordinals;
        std::map<std::string, std::vector<std::uint32_t>, std::less<>> sbac_slot_ordinals;
        std::map<std::string, std::size_t, std::less<>> next_sbac_slot;
        if (const auto *sample_bank = std::get_if<CurrentSbac>(&object->object.payload)) {
            for (std::size_t index = 0; index < sample_bank->slots.size(); ++index) {
                if (!sample_bank->slots[index].name.empty())
                    sbac_slot_ordinals[sample_bank->slots[index].name].push_back(static_cast<std::uint32_t>(index));
            }
        }
        for (const auto *relationship : *required) {
            const auto found = objects.find(*relationship->target_key);
            if (found == objects.end())
                return std::unexpected{package_error("package relationship target is absent from catalog")};
            std::uint32_t ordinal{};
            if (relationship->assignment_index) {
                ordinal = static_cast<std::uint32_t>(*relationship->assignment_index);
            } else if (relationship->type == "SBAC_SLOT_TO_SBNK") {
                const auto &target_name = found->second->object.header.name;
                const auto positions = sbac_slot_ordinals.find(target_name);
                auto &next = next_sbac_slot[target_name];
                if (positions == sbac_slot_ordinals.end() || next >= positions->second.size()) {
                    return std::unexpected{package_error("package SBAC relationship has no "
                                                         "matching source slot")};
                }
                ordinal = positions->second[next++];
            } else {
                ordinal = role_ordinals[relationship->type]++;
            }
            included_relationships.emplace_back(relationship, ordinal);
            queue.push_back(found->second);
        }
    }

    std::vector<ProvisionalNode> provisional;
    provisional.reserve(included_keys.size());
    for (const auto &key : included_keys) {
        const auto *snapshot = objects.at(key);
        auto profile = package_internal::build_relocation_profile(snapshot->object, snapshot->raw_payload);
        if (!profile)
            return std::unexpected{profile.error()};
        const auto payload_digest = package_internal::hex_digest(package_internal::sha256(snapshot->raw_payload));
        const auto normalized_digest =
            package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
        auto digests = waveform_digests(snapshot->object, snapshot->raw_payload, normalized_digest);
        if (!digests)
            return std::unexpected{digests.error()};
        provisional.push_back({snapshot, std::move(*profile), payload_digest, normalized_digest,
                               identity_digest(*snapshot, normalized_digest),
                               *digests ? std::optional{(*digests)->semantic} : std::nullopt,
                               *digests ? std::optional{(*digests)->audio} : std::nullopt});
    }
    const auto node_ids = assign_node_ids(provisional);

    PortablePackage package;
    package.schema_version = std::string{schema_version};
    package.source_media_kind = media_kind_name(source.kind());
    for (const auto &node : provisional) {
        PackageNode packaged;
        packaged.node_id = node_ids.at(node.snapshot->key);
        packaged.object_type = object_type_name(node.snapshot->object.header.type);
        packaged.object_format = object_format_name(node.snapshot->object.format);
        packaged.name = node.snapshot->object.header.name;
        packaged.payload_sha256 = node.payload_digest;
        packaged.payload_path = std::format("payloads/sha256/{}.bin", node.payload_digest);
        packaged.normalized_sha256 = node.normalized_digest;
        packaged.semantic_sha256 = node.semantic_digest;
        packaged.audio_sha256 = node.audio_digest;
        packaged.placement_hint = placement_hint(*node.snapshot);
        packaged.relocations = node.profile.relocations;
        packaged.payload_size_bytes = node.snapshot->raw_payload.size();
        packaged.raw_payload = node.snapshot->raw_payload;
        package.nodes.push_back(std::move(packaged));
        if (package_internal::is_opaque_sequence(node.snapshot->object)) {
            const auto semantic = decode_object(node.snapshot->raw_payload);
            if (semantic)
                return std::unexpected{package_error("opaque Sequence unexpectedly passed semantic decoding")};
            package.issues.push_back(
                {"SEQUENCE_PAYLOAD_PRESERVED_OPAQUE",
                 std::format("Sequence '{}' could not be decoded and was preserved byte-for-byte; MIDI conversion "
                             "and sampler playability are not verified: {}",
                             node.snapshot->object.header.name, semantic.error().message),
                 false});
        }
    }
    std::ranges::sort(package.nodes, {}, &PackageNode::node_id);

    for (const auto &[relationship, ordinal] : included_relationships) {
        const auto source_id = node_ids.at(relationship->source_key);
        const auto target_id = node_ids.at(*relationship->target_key);
        package.relationships.push_back({edge_id(source_id, target_id, relationship->type, ordinal), source_id,
                                         target_id, relationship->type, ordinal});
    }
    std::ranges::sort(package.relationships, {}, &PackageRelationship::edge_id);
    package_internal::bind_manifest_relocations(package);

    for (const auto &root : selected) {
        PackageRoot packaged{root.kind, root.display_name, {}};
        for (const auto *seed : root.seeds)
            packaged.node_ids.push_back(node_ids.at(seed->key));
        package.roots.push_back(std::move(packaged));
    }
    package.kind = derive_kind(package.roots);
    package.package_id = digest_text(package_internal::canonical_json(package_internal::manifest_json(package, false)));
    package.payloads_verified = true;
    if (const auto verified = verify_portable_package(package); !verified)
        return std::unexpected{verified.error()};
    const auto manifest = package_internal::canonical_json(package_internal::manifest_json(package, true));

    std::map<std::string, std::vector<std::byte>, std::less<>> payloads;
    for (const auto &node : package.nodes) {
        const auto [found, inserted] = payloads.emplace(node.payload_path, node.raw_payload);
        if (!inserted && found->second != node.raw_payload)
            return std::unexpected{package_error("package payload digest collision")};
    }
    std::vector<package_internal::ArchiveEntry> archive_entries;
    archive_entries.push_back({"manifest.json", string_bytes(manifest)});
    for (auto &[path, bytes] : payloads)
        archive_entries.push_back({std::move(path), std::move(bytes)});
    auto archive = package_internal::write_archive(std::move(archive_entries));
    if (!archive)
        return std::unexpected{archive.error()};
    const auto extension = std::string{required_package_extension(package.kind)};
    return PackageBuild{std::move(package), extension, std::move(*archive)};
}

std::map<std::string, const ObjectSnapshot *, std::less<>> catalog_objects(const ObjectCatalog &catalog) {
    std::map<std::string, const ObjectSnapshot *, std::less<>> objects;
    for (const auto &object : catalog.objects)
        objects.emplace(object.key, &object);
    return objects;
}

} // namespace

Result<PackageBuild> build_portable_package(const MediaContainer &source,
                                            std::span<const PackageRootSelector> root_selectors,
                                            const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    auto catalog = build_object_catalog(source, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    auto selected = select_roots(*catalog, root_selectors);
    if (!selected)
        return std::unexpected{selected.error()};
    const auto graph = build_relationship_graph(*catalog);
    const auto objects = catalog_objects(*catalog);
    return build_selected_package(source, *selected, graph, objects, cancellation);
}

Result<PackageBatchBuild> build_portable_packages(const MediaContainer &source,
                                                  std::span<const PackageRootSelector> root_selectors,
                                                  const CancellationToken &cancellation) {
    if (root_selectors.empty())
        return std::unexpected{
            package_error("at least one package root selector is required", ErrorCode::invalid_argument)};
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    auto catalog = build_object_catalog(source, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    const auto graph = build_relationship_graph(*catalog);
    const auto objects = catalog_objects(*catalog);

    PackageBatchBuild result;
    result.packages.reserve(root_selectors.size());
    for (std::size_t index = 0; index < root_selectors.size(); ++index) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto selector = root_selectors.subspan(index, 1U);
        auto selected = select_roots(*catalog, selector);
        if (!selected) {
            result.failures.push_back({index, selected.error()});
            continue;
        }
        auto package = build_selected_package(source, *selected, graph, objects, cancellation);
        if (!package) {
            if (package.error().code == ErrorCode::operation_cancelled)
                return std::unexpected{package.error()};
            result.failures.push_back({index, package.error()});
            continue;
        }
        result.packages.push_back({index, std::move(*package)});
    }
    return result;
}

} // namespace axk
