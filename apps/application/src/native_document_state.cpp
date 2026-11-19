#include "native_document_state.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace axk::app {
void NativeDocumentState::index_native_catalog() {
    const auto &catalog = native_document->catalog();
    content_children.emplace("", std::vector<std::size_t>{});
    std::unordered_map<std::string, std::size_t> nodes;
    std::unordered_map<std::string, std::vector<std::size_t>> object_nodes;
    for (const auto &node : catalog.nodes) {
        ImageContentItem item{};
        item.id = node.id;
        item.parent_id = node.parent_id;
        item.object_id = node.object_id;
        item.kind = node.type;
        item.name = item.display_name = node.name;
        item.scope_role = node.type;
        item.quality = "native";
        nodes.emplace(item.id, content.size());
        if (item.object_id)
            object_nodes[*item.object_id].push_back(content.size());
        content_children[item.parent_id.value_or("")].push_back(content.size());
        content_children.try_emplace(item.id);
        content.push_back(std::move(item));
    }
    for (auto &node : content) {
        node.child_count = content_children.at(node.id).size();
        auto parent = node.parent_id;
        while (parent) {
            ++node.depth;
            parent = content.at(nodes.at(*parent)).parent_id;
        }
    }
    root_count = content_children.at("").size();
    for (const auto &node : content)
        object_indices_by_content_scope.try_emplace(node.id);
    for (const auto &object : catalog.objects) {
        ImageObjectItem item{};
        item.id = object.id;
        if (object.type != "native-file")
            native_object_ids.emplace(object.id, object.id);
        item.type = object.type;
        item.name = object.name;
        item.format = std::string{format_id()};
        for (const auto &location : object.locations)
            item.stored_size_bytes += location.length;
        object_indices_by_id.emplace(item.id, objects.size());
        object_indices_by_type[item.type].push_back(objects.size());
        for (const auto node : object_nodes[item.id]) {
            auto scope = std::optional{content[node].id};
            while (scope) {
                object_indices_by_content_scope[*scope].push_back(objects.size());
                scope = content.at(nodes.at(*scope)).parent_id;
            }
        }
        objects.push_back(std::move(item));
    }
    for (const auto &edge : catalog.relationships) {
        ImageRelationshipItem item{};
        item.id = edge.id;
        item.source_object_id = edge.source_id;
        item.target_object_id = edge.target_id;
        item.candidate_object_ids = edge.candidates;
        item.type = edge.type;
        item.quality = edge.resolution == native::Resolution::resolved    ? "resolved"
                       : edge.resolution == native::Resolution::ambiguous ? "ambiguous"
                                                                          : "missing";
        item.basis = edge.native_reference;
        relationships.push_back(std::move(item));
    }
    for (const auto &issue : catalog.diagnostics)
        validation.push_back({issue.code,
                              issue.severity == native::Severity::error     ? "error"
                              : issue.severity == native::Severity::warning ? "warning"
                                                                            : "info",
                              issue.message,
                              {},
                              issue.object_id});
}
} // namespace axk::app
