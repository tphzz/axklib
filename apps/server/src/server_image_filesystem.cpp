#include "server_application.hpp"

#include <limits>
#include <utility>

#include "authentication.hpp"
#include "server_support.hpp"

namespace axk::server::detail {
crow::response ServerApplication::image_filesystem_response(const crow::request &request, const std::string &image_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto value = [&](const char *key) -> std::string {
        const auto *raw = request.url_params.get(key);
        return raw == nullptr ? std::string{} : std::string{raw};
    };
    const auto revision = parse_unsigned(value("expectedRevision"));
    const auto offset = parse_unsigned(value("offset").empty() ? "0" : value("offset"));
    const auto limit = parse_unsigned(value("limit").empty() ? "200" : value("limit"));
    if (!revision || !offset || !limit || *offset > std::numeric_limits<std::size_t>::max() ||
        *limit > config_.maximum_page_size)
        return error_response(400, {"invalid_request", "Valid expectedRevision, offset and limit are required"}, id);
    const auto page = images_.filesystem(image_id, request_owner(request), *revision,
                                         {.parent_id = value("parentId"),
                                          .root_id = value("rootId"),
                                          .query = value("query"),
                                          .entry_id = value("entryId"),
                                          .object_id = value("objectId"),
                                          .content_scope_id = value("contentScopeId"),
                                          .offset = static_cast<std::size_t>(*offset),
                                          .limit = static_cast<std::size_t>(*limit)});
    if (!page)
        return error_response(status_for_error(page.error(), 400), page.error(), id);
    Json items = Json::array();
    for (const auto &entry : page->items) {
        Json attributes = Json::array();
        for (const auto &attribute : entry.attributes)
            attributes.push_back({{"code", attribute.code},
                                  {"label", attribute.label},
                                  {"value", attribute.value},
                                  {"description", attribute.description},
                                  {"summary", attribute.summary}});
        items.push_back({{"id", entry.id},
                         {"parentId", entry.parent_id ? Json(*entry.parent_id) : Json{}},
                         {"rootId", entry.root_id},
                         {"ancestorIds", entry.ancestor_ids},
                         {"name", entry.name},
                         {"path", entry.path},
                         {"kind", entry.kind == "root"        ? "ROOT"
                                  : entry.kind == "partition" ? "PARTITION"
                                  : entry.kind == "directory" ? "DIRECTORY"
                                                              : "FILE"},
                         {"sizeBytes", entry.size_bytes ? Json(*entry.size_bytes) : Json{}},
                         {"childCount", entry.child_count},
                         {"objectId", entry.object_id ? Json(*entry.object_id) : Json{}},
                         {"contentScopeId", entry.content_scope_id ? Json(*entry.content_scope_id) : Json{}},
                         {"interpretation", entry.interpretation},
                         {"storage", entry.storage},
                         {"filesystemMetadata", entry.filesystem_metadata},
                         {"rawAttributes", entry.raw_attributes},
                         {"attributes", std::move(attributes)},
                         {"issue", entry.issue}});
    }
    Json capabilities = Json::array();
    for (const auto &root : page->root_capabilities)
        capabilities.push_back({{"rootId", root.root_id},
                                {"createDirectory", root.create_directory},
                                {"putFile", root.put_file},
                                {"deleteEntry", root.delete_entry},
                                {"renameEntry", root.rename_entry},
                                {"maximumNameBytes", root.maximum_name_bytes},
                                {"namePolicy", root.name_policy},
                                {"supportedImports", root.supported_imports},
                                {"namePattern", root.name_pattern},
                                {"nameHint", root.name_hint}});
    return json_response(200,
                         {{"data",
                           {{"revision", page->revision},
                            {"available", page->available},
                            {"filesystemName", page->filesystem_name},
                            {"deviceView", page->device_view ? Json(*page->device_view) : Json{}},
                            {"items", std::move(items)},
                            {"totalCount", page->total_count},
                            {"rootCapabilities", std::move(capabilities)}}},
                          {"meta", {{"requestId", id}}}},
                         id);
}
} // namespace axk::server::detail
