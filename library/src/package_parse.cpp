#include "axklib/package.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/package_relocation.hpp"
#include "axklib/utf8.hpp"
#include "package_internal.hpp"
#include "package_manifest_internal.hpp"

namespace axk {
namespace {

using Json = nlohmann::json;
using package_internal::closure_relationship;
using package_internal::derive_kind;
using package_internal::digest_text;
using package_internal::edge_id;
using package_internal::lower_extension;
using package_internal::ManifestArchiveEntry;
using package_internal::object_format_name;
using package_internal::object_type_name;
using package_internal::package_error;
using package_internal::parse_object_type;
using package_internal::parse_package_kind;
using package_internal::parse_root_kind;
using package_internal::recognized_extension;
using package_internal::root_object_type;
using package_internal::schema_version;
using package_internal::string_bytes;
using package_internal::waveform_digests;

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

    std::vector<std::set<std::string, std::less<>>> root_closures;
    std::set<std::string, std::less<>> reachable;
    for (const auto &root : package.roots) {
        auto &closure = root_closures.emplace_back();
        std::vector<std::string> queue(root.node_ids.begin(), root.node_ids.end());
        for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
            if (!closure.emplace(queue[cursor]).second)
                continue;
            reachable.emplace(queue[cursor]);
            for (const auto *edge : package_children(package, queue[cursor]))
                queue.push_back(edge->target_node_id);
        }
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
                auto edges = package_children(package, node.node_id, role);
                std::erase_if(edges, [&](const PackageRelationship *edge) { return edge->ordinal != index; });
                const auto exact_target_present = std::ranges::any_of(package.nodes, [&](const PackageNode &target) {
                    if (target.object_type != object_type_name(type) || target.name != assignment.name)
                        return false;
                    return std::ranges::any_of(root_closures, [&](const auto &closure) {
                        return closure.contains(node.node_id) && closure.contains(target.node_id);
                    });
                });
                if (edges.empty() && !exact_target_present)
                    continue;
                if (auto valid = require_edge(role, assignment.name, type, static_cast<std::uint32_t>(index)); !valid) {
                    return valid;
                }
            }
        }
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
    package_internal::bind_manifest_relocations(expected);
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
        node.payload_size_bytes = found->second.size;
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

} // namespace

Result<PortablePackage>
package_internal::parse_package_manifest(std::span<const std::byte> manifest_bytes,
                                         const std::map<std::string, ManifestArchiveEntry, std::less<>> &entries,
                                         bool verify_payloads, std::string_view filename) {
    const std::string manifest_text(reinterpret_cast<const char *>(manifest_bytes.data()), manifest_bytes.size());
    if (!text::is_valid_utf8(manifest_text) || !manifest_text.ends_with('\n') || manifest_text.contains('\r')) {
        return std::unexpected{package_error("package manifest is not canonical UTF-8 JSON")};
    }
    try {
        const auto manifest = Json::parse(manifest_text);
        if (!json_has_only_integer_numbers(manifest) || package_internal::canonical_json(manifest) != manifest_text)
            return std::unexpected{package_error("package manifest JSON is not canonical")};
        auto identity_manifest = manifest;
        identity_manifest.erase("package_id");
        const auto declared_id = manifest.at("package_id").get<std::string>();
        if (!lowercase_sha256(declared_id) ||
            digest_text(package_internal::canonical_json(identity_manifest)) != declared_id) {
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

Result<void> verify_portable_package(const PortablePackage &package) {
    try {
        if (!package.payloads_verified)
            return std::unexpected{package_error("package payloads have not been fully verified")};
        if (std::ranges::any_of(package.issues, &PackageIssue::fatal))
            return std::unexpected{package_error("package contains a fatal verification issue")};
        auto manifest = package_internal::manifest_json(package, true);
        auto identity_manifest = manifest;
        identity_manifest.erase("package_id");
        if (digest_text(package_internal::canonical_json(identity_manifest)) != package.package_id)
            return std::unexpected{package_error("package identity does not match its manifest")};

        const auto manifest_bytes = string_bytes(package_internal::canonical_json(manifest));
        for (const auto &node : package.nodes) {
            if (node.raw_payload.size() != node.payload_size_bytes)
                return std::unexpected{package_error("package payload size differs from its declaration")};
        }
        std::map<std::string, ManifestArchiveEntry, std::less<>> entries;
        entries.emplace("manifest.json", ManifestArchiveEntry{manifest_bytes.size(), &manifest_bytes});
        for (const auto &node : package.nodes) {
            const auto [found, inserted] =
                entries.emplace(node.payload_path, ManifestArchiveEntry{node.raw_payload.size(), &node.raw_payload});
            if (!inserted && *found->second.bytes != node.raw_payload)
                return std::unexpected{package_error("package payload path has conflicting bytes")};
        }
        auto reparsed = parse_manifest(manifest, entries, true);
        if (!reparsed)
            return std::unexpected{reparsed.error()};
        if (package_internal::manifest_json(*reparsed, true) != manifest)
            return std::unexpected{package_error("package verification changed its manifest")};
        return {};
    } catch (const Json::exception &) {
        return std::unexpected{package_error("package contains an invalid structured value")};
    }
}

} // namespace axk
