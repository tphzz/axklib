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

namespace axk {

bool package_internal::portable_inactive_program_relationship(const Relationship &relationship) {
    return (relationship.assignment_state == AssignmentState::source_load ||
            relationship.assignment_state == AssignmentState::visible_off) &&
           relationship.quality == RelationshipQuality::known && relationship.target_key.has_value();
}

namespace {

using Json = nlohmann::json;

constexpr std::string_view schema_version = "1.0";
constexpr std::uint64_t maximum_package_file_bytes = 512U * 1024U * 1024U;

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

Error package_error(std::string message, ErrorCode code = ErrorCode::manifest_invalid) {
    return make_error(code, ErrorCategory::manifest, std::move(message));
}

Error root_error(const PackageRootSelector &selector, std::string message) {
    ErrorContext context;
    context.partition_index = selector.partition_index;
    if (!selector.volume_name.empty())
        context.volume_name = selector.volume_name;
    if (selector.kind != PackageRootKind::volume)
        context.object_name = selector.object_name;
    return make_error(ErrorCode::object_missing, ErrorCategory::manifest, std::move(message), std::move(context));
}

std::vector<std::byte> string_bytes(std::string_view value) {
    const auto source = std::as_bytes(std::span{value});
    return {source.begin(), source.end()};
}

std::string digest_text(std::string_view value) {
    return package_internal::hex_digest(package_internal::sha256(string_bytes(value)));
}

std::string object_type_name(ObjectType type) {
    switch (type) {
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    case ObjectType::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<ObjectType> parse_object_type(std::string_view value) {
    if (value == "SMPL")
        return ObjectType::smpl;
    if (value == "SBNK")
        return ObjectType::sbnk;
    if (value == "SBAC")
        return ObjectType::sbac;
    if (value == "PROG")
        return ObjectType::prog;
    if (value == "SEQU")
        return ObjectType::sequ;
    if (value == "PRF3")
        return ObjectType::prf3;
    return std::nullopt;
}

std::string object_format_name(ObjectFormat format) {
    switch (format) {
    case ObjectFormat::current:
        return "current";
    case ObjectFormat::alternating_byte:
        return "alternating-byte";
    case ObjectFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

std::string media_kind_name(MediaKind kind) {
    switch (kind) {
    case MediaKind::sfs:
        return "sfs";
    case MediaKind::fat12_floppy:
        return "fat12-floppy";
    case MediaKind::iso9660:
        return "iso9660";
    case MediaKind::standalone_object:
        return "standalone-object";
    }
    return "unknown";
}

std::optional<PackageRootKind> parse_root_kind(std::string_view value) {
    if (value == "volume")
        return PackageRootKind::volume;
    if (value == "prog")
        return PackageRootKind::prog;
    if (value == "sbac")
        return PackageRootKind::sbac;
    if (value == "sbnk")
        return PackageRootKind::sbnk;
    if (value == "smpl")
        return PackageRootKind::smpl;
    if (value == "sequ")
        return PackageRootKind::sequ;
    return std::nullopt;
}

std::optional<PackageKind> parse_package_kind(std::string_view value) {
    if (value == "volume")
        return PackageKind::volume;
    if (value == "program")
        return PackageKind::program;
    if (value == "sbac")
        return PackageKind::sbac;
    if (value == "sbnk")
        return PackageKind::sbnk;
    if (value == "smpl")
        return PackageKind::smpl;
    if (value == "sequence")
        return PackageKind::sequence;
    if (value == "bundle")
        return PackageKind::bundle;
    return std::nullopt;
}

PackageKind derive_kind(std::span<const PackageRoot> roots) {
    if (roots.size() != 1U)
        return PackageKind::bundle;
    switch (roots.front().kind) {
    case PackageRootKind::volume:
        return PackageKind::volume;
    case PackageRootKind::prog:
        return PackageKind::program;
    case PackageRootKind::sbac:
        return PackageKind::sbac;
    case PackageRootKind::sbnk:
        return PackageKind::sbnk;
    case PackageRootKind::smpl:
        return PackageKind::smpl;
    case PackageRootKind::sequ:
        return PackageKind::sequence;
    }
    return PackageKind::bundle;
}

ObjectType root_object_type(PackageRootKind kind) {
    switch (kind) {
    case PackageRootKind::prog:
        return ObjectType::prog;
    case PackageRootKind::sbac:
        return ObjectType::sbac;
    case PackageRootKind::sbnk:
        return ObjectType::sbnk;
    case PackageRootKind::smpl:
        return ObjectType::smpl;
    case PackageRootKind::sequ:
        return ObjectType::sequ;
    case PackageRootKind::volume:
        return ObjectType::unknown;
    }
    return ObjectType::unknown;
}

bool matches_scope(const ObjectSnapshot &object, const PackageRootSelector &selector) {
    if (!object.placement)
        return false;
    if (selector.partition_index && object.placement->partition.value != *selector.partition_index)
        return false;
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
                if (object.object.header.type != expected || object.object.header.name != selector.object_name ||
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

bool closure_relationship(std::string_view role) {
    return role == "SBNK_LEFT_MEMBER_TO_SMPL" || role == "SBNK_RIGHT_MEMBER_TO_SMPL" || role == "SBAC_SLOT_TO_SBNK" ||
           role == "PROG_ASSIGNMENT_TO_SBAC" || role == "PROG_ASSIGNMENT_TO_SBNK";
}

bool portable_program_assignment(AssignmentState state) {
    return state == AssignmentState::active || state == AssignmentState::source_load ||
           state == AssignmentState::visible_off;
}

Result<std::vector<const Relationship *>> required_relationships(const ObjectSnapshot &object,
                                                                 const RelationshipGraph &graph) {
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
                                              "package closure does not contain one relationship for every "
                                              "active SBAC slot")};
        }
        for (const auto *row : candidates) {
            if (row->type != "SBAC_SLOT_TO_SBNK" || row->quality != RelationshipQuality::known || !row->target_key) {
                return std::unexpected{make_error(ErrorCode::relationship_unresolved, ErrorCategory::relationship,
                                                  "package closure requires known SBAC member "
                                                  "relationships")};
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

struct WaveformDigests {
    std::string semantic;
    std::string audio;
};

Result<std::optional<WaveformDigests>> waveform_digests(const DecodedObject &decoded,
                                                        std::span<const std::byte> raw_payload,
                                                        std::string_view normalized_digest) {
    const auto *smpl = std::get_if<CurrentSmpl>(&decoded.payload);
    if (smpl == nullptr)
        return std::optional<WaveformDigests>{};
    MediaObject media_object;
    media_object.decoded = decoded;
    media_object.raw_payload.assign(raw_payload.begin(), raw_payload.end());
    auto waveform = decode_waveform(media_object);
    if (!waveform)
        return std::unexpected{waveform.error()};
    const auto audio = package_internal::hex_digest(package_internal::sha256(waveform->pcm));
    const auto optional_u64 = [](const std::optional<std::uint64_t> &value) -> Json {
        return value ? Json(*value) : Json(nullptr);
    };
    const Json semantic{{"audio_sha256", audio},
                        {"decoded_sample_width_bytes", waveform->format.sample_width_bytes},
                        {"fine_tune_cents", smpl->fine_tune_cents.value},
                        {"frame_count", waveform->frame_count},
                        {"loop_end_frame_exclusive", optional_u64(smpl->loop_end_frame_exclusive)},
                        {"loop_end_frame_inclusive", optional_u64(smpl->loop_end_frame_inclusive)},
                        {"loop_length_frames", smpl->loop_length_frames.value},
                        {"loop_mode", smpl->loop_mode.value},
                        {"loop_start_frame", smpl->loop_start_frame.value},
                        {"name", decoded.header.name},
                        {"normalized_sha256", normalized_digest},
                        {"object_format", object_format_name(decoded.format)},
                        {"root_key", smpl->root_key.value},
                        {"sample_rate", smpl->sample_rate.value},
                        {"schema", "axklib-smpl-semantic-v1"},
                        {"source_wave_name", smpl->source_wave_name.value},
                        {"stored_pcm_bytes", smpl->stored_pcm_bytes},
                        {"stored_pcm_offset", smpl->stored_pcm_offset},
                        {"stored_sample_width_bytes", smpl->stored_sample_width_bytes.value},
                        {"wave_length_frames", smpl->wave_length_frames.value}};
    return std::optional{WaveformDigests{digest_text(semantic.dump() + '\n'), audio}};
}

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

std::string edge_id(std::string_view source, std::string_view target, std::string_view role, std::uint32_t ordinal) {
    std::string identity;
    const auto append = [&](std::string_view value) {
        identity += std::format("{}:", value.size());
        identity.append(value);
        identity.push_back(';');
    };
    append(source);
    append(target);
    append(role);
    append(std::to_string(ordinal));
    return "e-" + digest_text(identity);
}

Json relocation_json(const PackageRelocation &relocation) {
    return Json{{"edge_ids", relocation.edge_ids}, {"expected_hex", relocation.expected_hex},
                {"mask_hex", relocation.mask_hex}, {"offset", relocation.offset},
                {"role", relocation.role},         {"width", relocation.width}};
}

Json node_json(const PackageNode &node) {
    Json relocations = Json::array();
    for (const auto &relocation : node.relocations)
        relocations.push_back(relocation_json(relocation));
    const auto optional_digest = [](const std::optional<std::string> &value) -> Json {
        return value ? Json(*value) : Json(nullptr);
    };
    return Json{{"audio_sha256", optional_digest(node.audio_sha256)},
                {"name", node.name},
                {"node_id", node.node_id},
                {"normalized_sha256", node.normalized_sha256},
                {"object_format", node.object_format},
                {"object_type", node.object_type},
                {"payload_path", node.payload_path},
                {"payload_sha256", node.payload_sha256},
                {"placement_hint",
                 {{"category_name", node.placement_hint.category_name},
                  {"entry_name", node.placement_hint.entry_name},
                  {"group_name", node.placement_hint.group_name},
                  {"volume_name", node.placement_hint.volume_name}}},
                {"relocations", std::move(relocations)},
                {"semantic_sha256", optional_digest(node.semantic_sha256)}};
}

Json manifest_json(const PortablePackage &package, bool include_id) {
    Json roots = Json::array();
    for (const auto &root : package.roots)
        roots.push_back({{"display_name", root.display_name},
                         {"kind", package_root_kind_name(root.kind)},
                         {"node_ids", root.node_ids}});
    Json objects = Json::array();
    for (const auto &node : package.nodes)
        objects.push_back(node_json(node));
    Json relationships = Json::array();
    for (const auto &edge : package.relationships) {
        relationships.push_back({{"edge_id", edge.edge_id},
                                 {"ordinal", edge.ordinal},
                                 {"role", edge.role},
                                 {"source_node_id", edge.source_node_id},
                                 {"target_node_id", edge.target_node_id}});
    }
    std::map<std::string, std::pair<std::string, std::uint64_t>, std::less<>> payload_map;
    for (const auto &node : package.nodes)
        payload_map.emplace(node.payload_path,
                            std::pair{node.payload_sha256, static_cast<std::uint64_t>(node.raw_payload.size())});
    Json payloads = Json::array();
    for (const auto &[path, digest_and_size] : payload_map) {
        payloads.push_back({{"media_type", "application/vnd.axklib.yamaha-object"},
                            {"path", path},
                            {"sha256", digest_and_size.first},
                            {"size_bytes", digest_and_size.second}});
    }
    Json result{{"objects", std::move(objects)},
                {"package_kind", package_kind_name(package.kind)},
                {"payloads", std::move(payloads)},
                {"provenance", {{"source_media_kind", package.source_media_kind}}},
                {"relationships", std::move(relationships)},
                {"roots", std::move(roots)},
                {"schema_version", package.schema_version}};
    if (include_id)
        result["package_id"] = package.package_id;
    return result;
}

std::string canonical_json(const Json &value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::strict) + '\n';
}

void bind_relocations(PortablePackage &package) {
    for (auto &node : package.nodes) {
        for (auto &relocation : node.relocations) {
            for (const auto &edge : package.relationships) {
                const bool binds =
                    (relocation.role == "SBNK_LEFT_MEMBER_LINK" && edge.source_node_id == node.node_id &&
                     edge.role == "SBNK_LEFT_MEMBER_TO_SMPL") ||
                    (relocation.role == "SBNK_RIGHT_MEMBER_LINK" && edge.source_node_id == node.node_id &&
                     edge.role == "SBNK_RIGHT_MEMBER_TO_SMPL") ||
                    (relocation.role == "SBNK_GROUP_MEMBERSHIP" && edge.target_node_id == node.node_id &&
                     edge.role == "SBAC_SLOT_TO_SBNK") ||
                    (relocation.role == "SBAC_SLOT_HANDLE" && edge.source_node_id == node.node_id &&
                     edge.role == "SBAC_SLOT_TO_SBNK" && relocation.offset == 0x15cU + edge.ordinal * 0x14U) ||
                    (relocation.role == "PROG_ASSIGNMENT_HANDLE" && edge.source_node_id == node.node_id &&
                     (edge.role == "PROG_ASSIGNMENT_TO_SBAC" || edge.role == "PROG_ASSIGNMENT_TO_SBNK") &&
                     relocation.offset == 0x130U + edge.ordinal * 0x38U) ||
                    (relocation.role == "SBNK_PROGRAM_BITMAP" && edge.target_node_id == node.node_id &&
                     edge.role == "PROG_ASSIGNMENT_TO_SBNK");
                if (binds)
                    relocation.edge_ids.push_back(edge.edge_id);
            }
            std::ranges::sort(relocation.edge_ids);
        }
    }
}

bool json_has_only_integer_numbers(const Json &value) {
    if (value.is_number_float())
        return false;
    if (value.is_array() || value.is_object()) {
        for (const auto &child : value) {
            if (!json_has_only_integer_numbers(child))
                return false;
        }
    }
    return true;
}

bool exact_keys(const Json &value, std::initializer_list<std::string_view> expected) {
    if (!value.is_object() || value.size() != expected.size())
        return false;
    for (const auto key : expected) {
        if (!value.contains(key))
            return false;
    }
    return true;
}

bool lowercase_sha256(std::string_view value) {
    return value.size() == 64U && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

struct ManifestArchiveEntry {
    std::uint64_t size{};
    const std::vector<std::byte> *bytes{};
};

Result<PackageRelocation> parse_relocation(const Json &value) {
    if (!exact_keys(value, {"edge_ids", "expected_hex", "mask_hex", "offset", "role", "width"}))
        return std::unexpected{package_error("package relocation has unknown or missing fields")};
    PackageRelocation result;
    result.edge_ids = value.at("edge_ids").get<std::vector<std::string>>();
    result.expected_hex = value.at("expected_hex").get<std::string>();
    result.mask_hex = value.at("mask_hex").get<std::string>();
    result.offset = value.at("offset").get<std::uint32_t>();
    result.role = value.at("role").get<std::string>();
    result.width = value.at("width").get<std::uint32_t>();
    return result;
}

Result<void> validate_node_ids(const PortablePackage &package) {
    std::map<std::string, std::vector<std::string>, std::less<>> sample_banks;
    for (const auto &node : package.nodes) {
        std::string identity = node.object_type;
        identity.push_back('\0');
        identity += node.name;
        identity.push_back('\0');
        identity += node.normalized_sha256;
        sample_banks[digest_text(identity)].push_back(node.node_id);
    }
    for (auto &[identity, actual_ids] : sample_banks) {
        std::vector<std::string> expected_ids;
        if (actual_ids.size() == 1U) {
            expected_ids.push_back("n-" + identity);
        } else {
            for (std::size_t index = 0; index < actual_ids.size(); ++index)
                expected_ids.push_back("n-" + digest_text(std::format("{}#{}", identity, index + 1U)));
        }
        std::ranges::sort(actual_ids);
        std::ranges::sort(expected_ids);
        if (actual_ids != expected_ids)
            return std::unexpected{package_error("package object node ID is not deterministic")};
    }
    return {};
}

const PackageNode *node_by_id(const PortablePackage &package, std::string_view node_id) {
    const auto found = std::ranges::find(package.nodes, node_id, &PackageNode::node_id);
    return found == package.nodes.end() ? nullptr : &*found;
}

std::vector<const PackageRelationship *> package_children(const PortablePackage &package, std::string_view source,
                                                          std::string_view role = {}) {
    std::vector<const PackageRelationship *> result;
    for (const auto &edge : package.relationships) {
        if (edge.source_node_id == source && (role.empty() || edge.role == role))
            result.push_back(&edge);
    }
    return result;
}

Result<void> validate_manifest_graph(const PortablePackage &package) {
    for (const auto &root : package.roots) {
        if (root.kind != PackageRootKind::volume && root.node_ids.size() != 1U)
            return std::unexpected{package_error("single-object package root must contain one node")};
        if (root.kind == PackageRootKind::volume)
            continue;
        const auto *node = node_by_id(package, root.node_ids.front());
        if (node == nullptr || node->object_type != object_type_name(root_object_type(root.kind)))
            return std::unexpected{package_error("package root kind does not match its object node")};
    }

    std::set<std::string, std::less<>> reachable;
    std::vector<std::string> queue;
    for (const auto &root : package.roots)
        queue.insert(queue.end(), root.node_ids.begin(), root.node_ids.end());
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        if (!reachable.emplace(queue[cursor]).second)
            continue;
        for (const auto *edge : package_children(package, queue[cursor]))
            queue.push_back(edge->target_node_id);
    }
    if (reachable.size() != package.nodes.size())
        return std::unexpected{package_error("package contains an object unreachable from every root")};
    return {};
}

Result<void> validate_package_closure(const PortablePackage &package) {
    for (const auto &root : package.roots) {
        if (root.kind != PackageRootKind::volume && root.node_ids.size() != 1U)
            return std::unexpected{package_error("single-object package root must contain one node")};
        if (root.kind == PackageRootKind::volume)
            continue;
        const auto *node = node_by_id(package, root.node_ids.front());
        if (node == nullptr || node->object_type != object_type_name(root_object_type(root.kind)))
            return std::unexpected{package_error("package root kind does not match its object node")};
    }

    for (const auto &node : package.nodes) {
        const auto decoded = decode_object(node.raw_payload);
        if (!decoded)
            return std::unexpected{package_error("package object cannot be decoded for closure validation")};
        const auto require_edge = [&](std::string_view role, std::string_view target_name, ObjectType target_type,
                                      std::optional<std::uint32_t> ordinal = std::nullopt) -> Result<void> {
            auto edges = package_children(package, node.node_id, role);
            if (ordinal)
                std::erase_if(edges, [&](const PackageRelationship *edge) { return edge->ordinal != *ordinal; });
            if (edges.size() != 1U)
                return std::unexpected{
                    package_error(std::format("package closure requires one {} edge from {}", role, node.name))};
            const auto *target = node_by_id(package, edges.front()->target_node_id);
            if (target == nullptr || target->object_type != object_type_name(target_type) ||
                target->name != target_name) {
                return std::unexpected{package_error("package relationship target does not match "
                                                     "the raw object reference")};
            }
            return {};
        };

        if (const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload)) {
            if (!sample->left.wave_data_name.empty()) {
                if (auto valid =
                        require_edge("SBNK_LEFT_MEMBER_TO_SMPL", sample->left.wave_data_name, ObjectType::smpl);
                    !valid) {
                    return valid;
                }
            }
            if (sample->right && !sample->right->wave_data_name.empty()) {
                if (auto valid =
                        require_edge("SBNK_RIGHT_MEMBER_TO_SMPL", sample->right->wave_data_name, ObjectType::smpl);
                    !valid) {
                    return valid;
                }
            }
        } else if (const auto *sample_bank = std::get_if<CurrentSbac>(&decoded->payload)) {
            auto edges = package_children(package, node.node_id, "SBAC_SLOT_TO_SBNK");
            std::ranges::sort(edges, {}, &PackageRelationship::ordinal);
            const auto active_slots =
                std::ranges::count_if(sample_bank->slots, [](const SbacSlot &slot) { return !slot.name.empty(); });
            if (edges.size() != static_cast<std::size_t>(active_slots))
                return std::unexpected{package_error("package SBAC closure does not match its "
                                                     "active member slots")};
            std::set<std::uint32_t> ordinals;
            for (const auto *edge : edges) {
                const auto *target = node_by_id(package, edge->target_node_id);
                const auto valid_ordinal = edge->ordinal < sample_bank->slots.size();
                const auto expected_name =
                    valid_ordinal ? std::string_view{sample_bank->slots[edge->ordinal].name} : std::string_view{};
                if (!ordinals.emplace(edge->ordinal).second || expected_name.empty() || target == nullptr ||
                    target->object_type != "SBNK" || target->name != expected_name) {
                    return std::unexpected{package_error(
                        std::format("package SBAC slot {} requires SBNK '{}', but the edge "
                                    "targets '{}'",
                                    edge->ordinal, expected_name, target == nullptr ? "<missing>" : target->name))};
                }
            }
        } else if (const auto *program = std::get_if<CurrentProg>(&decoded->payload)) {
            for (std::size_t index = 0; index < program->assignments.size(); ++index) {
                const auto &assignment = program->assignments[index];
                if (assignment.name.empty() || std::to_integer<std::uint8_t>(assignment.raw_row[0x28U]) != 0xffU) {
                    continue;
                }
                const auto role = assignment.kind == 0x11U   ? "PROG_ASSIGNMENT_TO_SBAC"
                                  : assignment.kind == 0x10U ? "PROG_ASSIGNMENT_TO_SBNK"
                                                             : std::string_view{};
                const auto type = assignment.kind == 0x11U   ? ObjectType::sbac
                                  : assignment.kind == 0x10U ? ObjectType::sbnk
                                                             : ObjectType::unknown;
                if (role.empty())
                    return std::unexpected{package_error("package Program contains an unsupported "
                                                         "active assignment")};
                if (auto valid = require_edge(role, assignment.name, type, static_cast<std::uint32_t>(index)); !valid) {
                    return valid;
                }
            }
        }
    }

    std::set<std::string, std::less<>> reachable;
    std::vector<std::string> queue;
    for (const auto &root : package.roots)
        queue.insert(queue.end(), root.node_ids.begin(), root.node_ids.end());
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        if (!reachable.emplace(queue[cursor]).second)
            continue;
        for (const auto *edge : package_children(package, queue[cursor]))
            queue.push_back(edge->target_node_id);
    }
    if (reachable.size() != package.nodes.size())
        return std::unexpected{package_error("package contains an object unreachable from every root")};
    return {};
}

Result<void> validate_relocation_bindings(const PortablePackage &package) {
    auto expected = package;
    for (auto &node : expected.nodes) {
        for (auto &relocation : node.relocations)
            relocation.edge_ids.clear();
    }
    bind_relocations(expected);
    for (std::size_t node_index = 0; node_index < package.nodes.size(); ++node_index) {
        for (std::size_t relocation_index = 0; relocation_index < package.nodes[node_index].relocations.size();
             ++relocation_index) {
            if (package.nodes[node_index].relocations[relocation_index].edge_ids !=
                expected.nodes[node_index].relocations[relocation_index].edge_ids) {
                return std::unexpected{package_error("package relocation edge binding is invalid")};
            }
        }
    }
    return {};
}

Result<PortablePackage> parse_manifest(const Json &manifest,
                                       const std::map<std::string, ManifestArchiveEntry, std::less<>> &archive_entries,
                                       bool verify_payloads) {
    if (!exact_keys(manifest, {"objects", "package_id", "package_kind", "payloads", "provenance", "relationships",
                               "roots", "schema_version"})) {
        return std::unexpected{package_error("package manifest has unknown or missing top-level fields")};
    }
    PortablePackage result;
    result.schema_version = manifest.at("schema_version").get<std::string>();
    if (result.schema_version != schema_version)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "package schema version is unsupported")};
    result.package_id = manifest.at("package_id").get<std::string>();
    const auto parsed_kind = parse_package_kind(manifest.at("package_kind").get<std::string>());
    if (!parsed_kind)
        return std::unexpected{package_error("package kind is invalid")};
    result.kind = *parsed_kind;
    if (!exact_keys(manifest.at("provenance"), {"source_media_kind"}))
        return std::unexpected{package_error("package provenance has unknown or missing fields")};
    result.source_media_kind = manifest.at("provenance").at("source_media_kind").get<std::string>();

    std::map<std::string, std::string, std::less<>> declared_payloads;
    for (const auto &payload : manifest.at("payloads")) {
        if (!exact_keys(payload, {"media_type", "path", "sha256", "size_bytes"}) ||
            payload.at("media_type") != "application/vnd.axklib.yamaha-object") {
            return std::unexpected{package_error("package payload declaration is invalid")};
        }
        const auto path = payload.at("path").get<std::string>();
        const auto digest = payload.at("sha256").get<std::string>();
        const auto size = payload.at("size_bytes").get<std::uint64_t>();
        const auto found = archive_entries.find(path);
        if (!lowercase_sha256(digest) || !declared_payloads.emplace(path, digest).second ||
            found == archive_entries.end() || found->second.size != size ||
            (verify_payloads &&
             (found->second.bytes == nullptr ||
              package_internal::hex_digest(package_internal::sha256(*found->second.bytes)) != digest))) {
            return std::unexpected{package_error("package payload declaration does not match the archive")};
        }
    }
    if (declared_payloads.size() + 1U != archive_entries.size())
        return std::unexpected{package_error("package archive contains an undeclared entry")};

    for (const auto &root : manifest.at("roots")) {
        if (!exact_keys(root, {"display_name", "kind", "node_ids"}))
            return std::unexpected{package_error("package root has unknown or missing fields")};
        const auto kind = parse_root_kind(root.at("kind").get<std::string>());
        if (!kind)
            return std::unexpected{package_error("package root kind is invalid")};
        result.roots.push_back(
            {*kind, root.at("display_name").get<std::string>(), root.at("node_ids").get<std::vector<std::string>>()});
    }
    if (result.roots.empty() || derive_kind(result.roots) != result.kind)
        return std::unexpected{package_error("declared package kind does not match its roots")};

    std::set<std::string, std::less<>> node_ids;
    for (const auto &object : manifest.at("objects")) {
        if (!exact_keys(object,
                        {"audio_sha256", "name", "node_id", "normalized_sha256", "object_format", "object_type",
                         "payload_path", "payload_sha256", "placement_hint", "relocations", "semantic_sha256"}) ||
            !exact_keys(object.at("placement_hint"), {"category_name", "entry_name", "group_name", "volume_name"})) {
            return std::unexpected{package_error("package object has unknown or missing fields")};
        }
        PackageNode node;
        node.name = object.at("name").get<std::string>();
        node.node_id = object.at("node_id").get<std::string>();
        node.normalized_sha256 = object.at("normalized_sha256").get<std::string>();
        if (!object.at("semantic_sha256").is_null())
            node.semantic_sha256 = object.at("semantic_sha256").get<std::string>();
        if (!object.at("audio_sha256").is_null())
            node.audio_sha256 = object.at("audio_sha256").get<std::string>();
        node.object_format = object.at("object_format").get<std::string>();
        node.object_type = object.at("object_type").get<std::string>();
        node.payload_path = object.at("payload_path").get<std::string>();
        node.payload_sha256 = object.at("payload_sha256").get<std::string>();
        const auto &hint = object.at("placement_hint");
        node.placement_hint = {hint.at("group_name").get<std::string>(), hint.at("volume_name").get<std::string>(),
                               hint.at("category_name").get<std::string>(), hint.at("entry_name").get<std::string>()};
        for (const auto &relocation : object.at("relocations")) {
            auto parsed = parse_relocation(relocation);
            if (!parsed)
                return std::unexpected{parsed.error()};
            node.relocations.push_back(std::move(*parsed));
        }
        const auto found = archive_entries.find(node.payload_path);
        const auto declared = declared_payloads.find(node.payload_path);
        const auto type = parse_object_type(node.object_type);
        if (!node_ids.emplace(node.node_id).second || !type || found == archive_entries.end() ||
            declared == declared_payloads.end() || node.payload_sha256 != declared->second ||
            !lowercase_sha256(node.normalized_sha256) ||
            (node.semantic_sha256 && !lowercase_sha256(*node.semantic_sha256)) ||
            (node.audio_sha256 && !lowercase_sha256(*node.audio_sha256))) {
            return std::unexpected{package_error("package object identity or payload reference is invalid")};
        }
        if (verify_payloads) {
            if (found->second.bytes == nullptr)
                return std::unexpected{package_error("package payload bytes are unavailable")};
            node.raw_payload = *found->second.bytes;
            if (package_internal::hex_digest(package_internal::sha256(node.raw_payload)) != node.payload_sha256) {
                return std::unexpected{package_error("package object payload digest mismatch")};
            }
            const auto decoded = decode_object(node.raw_payload);
            if (!decoded || decoded->header.type != *type || decoded->header.name != node.name ||
                object_format_name(decoded->format) != node.object_format) {
                return std::unexpected{package_error("package object payload does not match its declaration")};
            }
            const auto profile = package_internal::build_relocation_profile(*decoded, node.raw_payload);
            if (!profile || package_internal::hex_digest(package_internal::sha256(profile->normalized_payload)) !=
                                node.normalized_sha256) {
                return std::unexpected{package_error("package object normalized identity is invalid")};
            }
            auto digests = waveform_digests(*decoded, node.raw_payload, node.normalized_sha256);
            if (!digests)
                return std::unexpected{digests.error()};
            const auto expected_semantic = *digests ? std::optional{(*digests)->semantic} : std::nullopt;
            const auto expected_audio = *digests ? std::optional{(*digests)->audio} : std::nullopt;
            if (node.semantic_sha256 != expected_semantic || node.audio_sha256 != expected_audio) {
                return std::unexpected{package_error("package object semantic identity is invalid")};
            }
            if (profile->relocations.size() != node.relocations.size())
                return std::unexpected{package_error("package object relocation registry is incomplete")};
            for (std::size_t index = 0; index < node.relocations.size(); ++index) {
                const auto &expected = profile->relocations[index];
                const auto &actual = node.relocations[index];
                if (expected.offset != actual.offset || expected.width != actual.width ||
                    expected.mask_hex != actual.mask_hex || expected.role != actual.role ||
                    expected.expected_hex != actual.expected_hex) {
                    return std::unexpected{package_error("package object relocation descriptor is invalid")};
                }
            }
        }
        result.nodes.push_back(std::move(node));
    }
    if (result.nodes.empty())
        return std::unexpected{package_error("package contains no object nodes")};

    std::set<std::string, std::less<>> edge_ids;
    for (const auto &relationship : manifest.at("relationships")) {
        if (!exact_keys(relationship, {"edge_id", "ordinal", "role", "source_node_id", "target_node_id"})) {
            return std::unexpected{package_error("package relationship has unknown or missing fields")};
        }
        PackageRelationship edge{
            relationship.at("edge_id").get<std::string>(), relationship.at("source_node_id").get<std::string>(),
            relationship.at("target_node_id").get<std::string>(), relationship.at("role").get<std::string>(),
            relationship.at("ordinal").get<std::uint32_t>()};
        if (!closure_relationship(edge.role) || !node_ids.contains(edge.source_node_id) ||
            !node_ids.contains(edge.target_node_id) || !edge_ids.emplace(edge.edge_id).second ||
            edge.edge_id != edge_id(edge.source_node_id, edge.target_node_id, edge.role, edge.ordinal)) {
            return std::unexpected{package_error("package relationship identity is invalid")};
        }
        result.relationships.push_back(std::move(edge));
    }
    for (const auto &root : result.roots) {
        if (root.node_ids.empty() ||
            std::ranges::any_of(root.node_ids, [&](const std::string &id) { return !node_ids.contains(id); })) {
            return std::unexpected{package_error("package root references an undeclared object node")};
        }
    }
    for (const auto &node : result.nodes) {
        for (const auto &relocation : node.relocations) {
            if (std::ranges::any_of(relocation.edge_ids,
                                    [&](const std::string &id) { return !edge_ids.contains(id); })) {
                return std::unexpected{package_error("package relocation references an undeclared edge")};
            }
        }
    }
    if (!std::ranges::is_sorted(result.nodes, {}, &PackageNode::node_id) ||
        !std::ranges::is_sorted(result.relationships, {}, &PackageRelationship::edge_id)) {
        return std::unexpected{package_error("package object or relationship order is not canonical")};
    }
    if (auto valid = validate_node_ids(result); !valid)
        return std::unexpected{valid.error()};
    if (auto valid = validate_manifest_graph(result); !valid)
        return std::unexpected{valid.error()};
    if (verify_payloads) {
        if (auto valid = validate_package_closure(result); !valid)
            return std::unexpected{valid.error()};
    }
    if (auto valid = validate_relocation_bindings(result); !valid)
        return std::unexpected{valid.error()};
    result.payloads_verified = verify_payloads;
    return result;
}

std::string lower_extension(std::string_view filename) {
    const auto slash = filename.find_last_of("/\\");
    const auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string result{filename.substr(dot)};
    std::ranges::transform(result, result.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

bool recognized_extension(std::string_view extension) {
    constexpr std::array extensions{".axkvol", ".axkprg", ".axksbac", ".axksbnk", ".axksmpl", ".axkseq", ".axkpkg"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

PackageKind derive_kind(std::span<const PackageRootSelector> roots) {
    if (roots.size() != 1U)
        return PackageKind::bundle;
    switch (roots.front().kind) {
    case PackageRootKind::volume:
        return PackageKind::volume;
    case PackageRootKind::prog:
        return PackageKind::program;
    case PackageRootKind::sbac:
        return PackageKind::sbac;
    case PackageRootKind::sbnk:
        return PackageKind::sbnk;
    case PackageRootKind::smpl:
        return PackageKind::smpl;
    case PackageRootKind::sequ:
        return PackageKind::sequence;
    }
    return PackageKind::bundle;
}

Result<std::filesystem::path> resolve_output_path(const std::filesystem::path &requested, PackageKind kind) {
    const auto path_text = text::path_to_utf8(requested);
    if (!text::is_valid_utf8(path_text)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package output path is not valid UTF-8")};
    }
    if (requested.empty() || requested.filename().empty() || requested.filename() == "." ||
        requested.filename() == "..") {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package output path must name a file")};
    }

    const auto required = required_package_extension(kind);
    const auto extension = lower_extension(text::path_to_utf8(requested.filename()));
    if (!extension.empty()) {
        if (extension != required) {
            const auto qualifier = recognized_extension(extension) ? "recognized package" : "unrelated";
            return std::unexpected{make_error(
                ErrorCode::invalid_argument, ErrorCategory::io,
                std::format("package output has {} extension {}; {} is required", qualifier, extension, required))};
        }
        return requested;
    }

    auto suffix = text::path_from_utf8(required);
    if (!suffix)
        return std::unexpected{suffix.error()};
    auto result = requested;
    result += suffix->native();
    return result;
}

Result<void> preflight_output(const std::filesystem::path &path, bool overwrite) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not inspect package output path")};
    }
    if (!overwrite && exists) {
        ErrorContext context;
        context.source_path = text::path_to_utf8(path);
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "refusing to replace an existing package", std::move(context))};
    }
    return {};
}

class TemporaryPackageCleanup {
  public:
    explicit TemporaryPackageCleanup(std::filesystem::path path) : path_{std::move(path)} {}
    ~TemporaryPackageCleanup() {
        if (active_)
            detail::discard_temporary_file(path_);
    }
    TemporaryPackageCleanup(const TemporaryPackageCleanup &) = delete;
    TemporaryPackageCleanup &operator=(const TemporaryPackageCleanup &) = delete;
    void release() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

Result<std::filesystem::path> reserve_unique_temporary(const std::filesystem::path &output) {
    return detail::reserve_temporary_file(output);
}

Result<void> flush_package_file(const std::filesystem::path &path) { return detail::flush_file_to_disk(path); }

Result<void> publish_temporary_package(const std::filesystem::path &temporary, const std::filesystem::path &output,
                                       bool overwrite) {
    return detail::publish_temporary_file(temporary, output, overwrite);
}

Result<std::vector<std::byte>> read_package_reader(const RandomAccessReader &reader,
                                                   const CancellationToken &cancellation);

Result<std::vector<std::byte>> read_package_file(const std::filesystem::path &path,
                                                 const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return read_package_reader(**reader, cancellation);
}

Result<std::vector<std::byte>> read_package_reader(const RandomAccessReader &reader,
                                                   const CancellationToken &cancellation) {
    if (reader.size() > maximum_package_file_bytes ||
        reader.size() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                          "package file exceeds the configured archive limit")};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(reader.size()));
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (const auto read = reader.read_exact_at(0U, bytes); !read)
        return std::unexpected{read.error()};
    return bytes;
}

Result<PortablePackage> parse_package_manifest(std::span<const std::byte> manifest_bytes,
                                               const std::map<std::string, ManifestArchiveEntry, std::less<>> &entries,
                                               bool verify_payloads, std::string_view filename) {
    const std::string manifest_text(reinterpret_cast<const char *>(manifest_bytes.data()), manifest_bytes.size());
    if (!text::is_valid_utf8(manifest_text) || !manifest_text.ends_with('\n') || manifest_text.contains('\r')) {
        return std::unexpected{package_error("package manifest is not canonical UTF-8 JSON")};
    }
    try {
        const auto manifest = Json::parse(manifest_text);
        if (!json_has_only_integer_numbers(manifest) || canonical_json(manifest) != manifest_text)
            return std::unexpected{package_error("package manifest JSON is not canonical")};
        auto identity_manifest = manifest;
        identity_manifest.erase("package_id");
        const auto declared_id = manifest.at("package_id").get<std::string>();
        if (!lowercase_sha256(declared_id) || digest_text(canonical_json(identity_manifest)) != declared_id) {
            return std::unexpected{package_error("package ID does not match the canonical manifest")};
        }
        auto package = parse_manifest(manifest, entries, verify_payloads);
        if (!package)
            return std::unexpected{package.error()};
        const auto extension = lower_extension(filename);
        if (!extension.empty() && recognized_extension(extension) &&
            extension != required_package_extension(package->kind)) {
            package->issues.push_back({"PACKAGE_EXTENSION_MISMATCH",
                                       std::format("filename extension {} disagrees with manifest "
                                                   "package kind {}",
                                                   extension, package_kind_name(package->kind)),
                                       false});
        }
        return package;
    } catch (const Json::exception &) {
        return std::unexpected{package_error("package manifest JSON is malformed")};
    } catch (const std::exception &) {
        return std::unexpected{package_error("package manifest contains an invalid typed value")};
    }
}

} // namespace

std::string_view package_root_kind_name(PackageRootKind kind) noexcept {
    switch (kind) {
    case PackageRootKind::volume:
        return "volume";
    case PackageRootKind::prog:
        return "prog";
    case PackageRootKind::sbac:
        return "sbac";
    case PackageRootKind::sbnk:
        return "sbnk";
    case PackageRootKind::smpl:
        return "smpl";
    case PackageRootKind::sequ:
        return "sequ";
    }
    return "volume";
}

std::string_view package_kind_name(PackageKind kind) noexcept {
    switch (kind) {
    case PackageKind::volume:
        return "volume";
    case PackageKind::program:
        return "program";
    case PackageKind::sbac:
        return "sbac";
    case PackageKind::sbnk:
        return "sbnk";
    case PackageKind::smpl:
        return "smpl";
    case PackageKind::sequence:
        return "sequence";
    case PackageKind::bundle:
        return "bundle";
    }
    return "bundle";
}

std::string_view required_package_extension(PackageKind kind) noexcept {
    switch (kind) {
    case PackageKind::volume:
        return ".axkvol";
    case PackageKind::program:
        return ".axkprg";
    case PackageKind::sbac:
        return ".axksbac";
    case PackageKind::sbnk:
        return ".axksbnk";
    case PackageKind::smpl:
        return ".axksmpl";
    case PackageKind::sequence:
        return ".axkseq";
    case PackageKind::bundle:
        return ".axkpkg";
    }
    return ".axkpkg";
}

std::string_view package_import_action_name(PackageImportObjectAction action) noexcept {
    switch (action) {
    case PackageImportObjectAction::reuse:
        return "reuse";
    case PackageImportObjectAction::rename:
        return "rename";
    case PackageImportObjectAction::relocate:
        return "relocate";
    case PackageImportObjectAction::insert:
        return "insert";
    case PackageImportObjectAction::conflict:
        return "conflict";
    }
    return "conflict";
}

Result<void> verify_portable_package(const PortablePackage &package) {
    try {
        if (!package.payloads_verified)
            return std::unexpected{package_error("package payloads have not been fully verified")};
        if (std::ranges::any_of(package.issues, &PackageIssue::fatal))
            return std::unexpected{package_error("package contains a fatal verification issue")};
        auto manifest = manifest_json(package, true);
        auto identity_manifest = manifest;
        identity_manifest.erase("package_id");
        if (digest_text(canonical_json(identity_manifest)) != package.package_id)
            return std::unexpected{package_error("package identity does not match its manifest")};

        std::map<std::string, std::vector<std::byte>, std::less<>> entry_bytes;
        entry_bytes.emplace("manifest.json", string_bytes(canonical_json(manifest)));
        for (const auto &node : package.nodes) {
            const auto [found, inserted] = entry_bytes.emplace(node.payload_path, node.raw_payload);
            if (!inserted && found->second != node.raw_payload)
                return std::unexpected{package_error("package payload path has conflicting bytes")};
        }
        std::map<std::string, ManifestArchiveEntry, std::less<>> entries;
        for (const auto &[path, bytes] : entry_bytes)
            entries.emplace(path, ManifestArchiveEntry{bytes.size(), &bytes});
        auto reparsed = parse_manifest(manifest, entries, true);
        if (!reparsed)
            return std::unexpected{reparsed.error()};
        if (manifest_json(*reparsed, true) != manifest)
            return std::unexpected{package_error("package verification changed its manifest")};
        return {};
    } catch (const Json::exception &) {
        return std::unexpected{package_error("package contains an invalid structured value")};
    }
}

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
    std::map<std::string, const ObjectSnapshot *, std::less<>> objects;
    for (const auto &object : catalog->objects)
        objects.emplace(object.key, &object);

    std::set<std::string, std::less<>> included_keys;
    std::vector<std::pair<const Relationship *, std::uint32_t>> included_relationships;
    std::vector<const ObjectSnapshot *> queue;
    for (const auto &root : *selected)
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
        auto required = required_relationships(*object, graph);
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
        packaged.raw_payload = node.snapshot->raw_payload;
        package.nodes.push_back(std::move(packaged));
    }
    std::ranges::sort(package.nodes, {}, &PackageNode::node_id);

    for (const auto &[relationship, ordinal] : included_relationships) {
        const auto source_id = node_ids.at(relationship->source_key);
        const auto target_id = node_ids.at(*relationship->target_key);
        package.relationships.push_back({edge_id(source_id, target_id, relationship->type, ordinal), source_id,
                                         target_id, relationship->type, ordinal});
    }
    std::ranges::sort(package.relationships, {}, &PackageRelationship::edge_id);
    bind_relocations(package);

    for (const auto &root : *selected) {
        PackageRoot packaged{root.kind, root.display_name, {}};
        for (const auto *seed : root.seeds)
            packaged.node_ids.push_back(node_ids.at(seed->key));
        package.roots.push_back(std::move(packaged));
    }
    package.kind = derive_kind(package.roots);
    package.package_id = digest_text(canonical_json(manifest_json(package, false)));
    package.payloads_verified = true;
    if (const auto verified = verify_portable_package(package); !verified)
        return std::unexpected{verified.error()};
    const auto manifest = canonical_json(manifest_json(package, true));

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

Result<PortablePackage> open_portable_package(std::span<const std::byte> archive, std::string_view filename) {
    auto entries = package_internal::read_archive(archive);
    if (!entries)
        return std::unexpected{entries.error()};
    std::map<std::string, std::vector<std::byte>, std::less<>> entry_bytes;
    for (auto &entry : *entries)
        entry_bytes.emplace(std::move(entry.path), std::move(entry.bytes));
    const auto manifest_entry = entry_bytes.find("manifest.json");
    if (manifest_entry == entry_bytes.end())
        return std::unexpected{package_error("package manifest is absent")};
    std::map<std::string, ManifestArchiveEntry, std::less<>> descriptors;
    for (const auto &[path, bytes] : entry_bytes)
        descriptors.emplace(path, ManifestArchiveEntry{bytes.size(), &bytes});
    return parse_package_manifest(manifest_entry->second, descriptors, true, filename);
}

Result<PackagePublication> publish_portable_package(const PackageBuild &build, const std::filesystem::path &output_path,
                                                    bool overwrite, const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    const auto required = std::string{required_package_extension(build.package.kind)};
    if (build.required_extension != required) {
        return std::unexpected{package_error("package build required extension is inconsistent")};
    }
    auto resolved = resolve_output_path(output_path, build.package.kind);
    if (!resolved)
        return std::unexpected{resolved.error()};
    if (const auto available = preflight_output(*resolved, overwrite); !available)
        return std::unexpected{available.error()};
    auto verified = open_portable_package(build.archive, text::path_to_utf8(resolved->filename()));
    if (!verified)
        return std::unexpected{verified.error()};
    if (verified->package_id != build.package.package_id || verified->kind != build.package.kind) {
        return std::unexpected{package_error("package build metadata disagrees with its archive")};
    }

    std::error_code filesystem_error;
    if (!resolved->parent_path().empty())
        std::filesystem::create_directories(resolved->parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create package output directory")};
    }
    auto temporary = reserve_unique_temporary(*resolved);
    if (!temporary)
        return std::unexpected{temporary.error()};
    TemporaryPackageCleanup cleanup{*temporary};
    if (auto resized = detail::resize_temporary_file(*temporary, build.archive.size()); !resized)
        return std::unexpected{resized.error()};
    if (auto written = detail::write_temporary_file_at(*temporary, 0U, build.archive); !written)
        return std::unexpected{written.error()};
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (const auto flushed = flush_package_file(*temporary); !flushed)
        return std::unexpected{flushed.error()};
    auto reopened = open_portable_package(*temporary, cancellation);
    if (!reopened)
        return std::unexpected{reopened.error()};
    if (reopened->package_id != build.package.package_id) {
        return std::unexpected{package_error("temporary package failed identity verification")};
    }
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (const auto available = preflight_output(*resolved, overwrite); !available)
        return std::unexpected{available.error()};
    if (const auto published = publish_temporary_package(*temporary, *resolved, overwrite); !published) {
        return std::unexpected{published.error()};
    }
    cleanup.release();
    return PackagePublication{*resolved, build.package.package_id, build.package.kind,
                              static_cast<std::uint64_t>(build.archive.size())};
}

Result<PackagePublication> export_portable_package(const MediaContainer &source,
                                                   std::span<const PackageRootSelector> roots,
                                                   const std::filesystem::path &output_path, bool overwrite,
                                                   const CancellationToken &cancellation) {
    if (roots.empty()) {
        return std::unexpected{
            package_error("at least one package root selector is required", ErrorCode::invalid_argument)};
    }
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    const auto expected_kind = derive_kind(roots);
    auto resolved = resolve_output_path(output_path, expected_kind);
    if (!resolved)
        return std::unexpected{resolved.error()};
    if (const auto available = preflight_output(*resolved, overwrite); !available)
        return std::unexpected{available.error()};
    auto build = build_portable_package(source, roots, cancellation);
    if (!build)
        return std::unexpected{build.error()};
    if (build->package.kind != expected_kind) {
        return std::unexpected{package_error("resolved package roots changed the requested root kind")};
    }
    return publish_portable_package(*build, *resolved, overwrite, cancellation);
}

Result<PortablePackage> open_portable_package(const std::filesystem::path &path,
                                              const CancellationToken &cancellation) {
    const auto filename = text::path_to_utf8(path.filename());
    return open_portable_package(path, filename, cancellation);
}

Result<PortablePackage> open_portable_package(const std::filesystem::path &path, std::string_view filename,
                                              const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto archive = read_package_file(path, cancellation);
    if (!archive)
        return std::unexpected{archive.error()};
    return open_portable_package(*archive, filename);
}

Result<PortablePackage> open_portable_package(const RandomAccessReader &reader, std::string_view filename,
                                              const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto archive = read_package_reader(reader, cancellation);
    if (!archive)
        return std::unexpected{archive.error()};
    return open_portable_package(*archive, filename);
}

Result<PortablePackage> inspect_portable_package(const std::filesystem::path &path,
                                                 const CancellationToken &cancellation) {
    const auto filename = text::path_to_utf8(path.filename());
    return inspect_portable_package(path, filename, cancellation);
}

Result<PortablePackage> inspect_portable_package(const std::filesystem::path &path, std::string_view filename,
                                                 const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto inspected = package_internal::inspect_archive(**reader, cancellation);
    if (!inspected)
        return std::unexpected{inspected.error()};
    std::map<std::string, ManifestArchiveEntry, std::less<>> descriptors;
    for (const auto &entry : inspected->entries)
        descriptors.emplace(entry.path, ManifestArchiveEntry{entry.size, nullptr});
    return parse_package_manifest(inspected->manifest.bytes, descriptors, false, filename);
}

Result<PortablePackage> inspect_portable_package(const RandomAccessReader &reader, std::string_view filename,
                                                 const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto inspected = package_internal::inspect_archive(reader, cancellation);
    if (!inspected)
        return std::unexpected{inspected.error()};
    std::map<std::string, ManifestArchiveEntry, std::less<>> descriptors;
    for (const auto &entry : inspected->entries)
        descriptors.emplace(entry.path, ManifestArchiveEntry{entry.size, nullptr});
    return parse_package_manifest(inspected->manifest.bytes, descriptors, false, filename);
}

} // namespace axk
