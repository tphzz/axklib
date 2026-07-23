#include "package_manifest_internal.hpp"

#include <algorithm>
#include <map>
#include <ranges>

#include <nlohmann/json.hpp>

namespace axk {
namespace {

using Json = nlohmann::json;

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

} // namespace

Json package_internal::manifest_json(const PortablePackage &package, bool include_id) {
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

std::string package_internal::canonical_json(const Json &value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::strict) + '\n';
}

void package_internal::bind_manifest_relocations(PortablePackage &package) {
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

} // namespace axk
