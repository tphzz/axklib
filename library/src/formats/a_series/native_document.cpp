#include "axklib/formats/a_series/native_document.hpp"

#include <optional>
#include <utility>

namespace axk::formats::a_series {

NativeDocument::NativeDocument(std::shared_ptr<const a_series::Document> decoded,
                               std::shared_ptr<const native::SourceSet> sources, native::DocumentSummary summary,
                               native::Catalog catalog)
    : decoded_(std::move(decoded)), sources_(std::move(sources)), summary_(std::move(summary)),
      catalog_(std::move(catalog)) {}

Result<std::shared_ptr<const NativeDocument>> NativeDocument::create(std::shared_ptr<const a_series::Document> decoded,
                                                                     std::shared_ptr<const native::SourceSet> sources,
                                                                     std::string id, Locations locations) {
    if (!decoded || !sources || id.empty())
        return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "Native document requires a decoded owner, admitted sources and identity"));
    native::DocumentSummary summary{
        std::move(id),         "yamaha.a-series", "a-series", "A-series", native::Completeness::complete,
        {{"browse", true, {}}}};
    native::Catalog catalog;
    const auto &state = decoded->state();
    for (const auto &object : state.inventory.catalog.objects) {
        auto &bound = locations[object.key];
        for (const auto &location : bound) {
            auto file = sources->locate(location.member_id, location.root_id, location.entry_id);
            if (!file || location.offset > file->length || location.length > file->length - location.offset)
                return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                                  "Object location is outside its admitted source"));
        }
        catalog.objects.push_back({object.key,
                                   object.object.header.raw_type,
                                   object.object.header.name,
                                   std::move(bound),
                                   {{"inspect", true, {}}}});
    }
    for (const auto &edge : state.graph.relationships) {
        const auto resolution = edge.target_key                   ? native::Resolution::resolved
                                : edge.candidate_keys.size() > 1U ? native::Resolution::ambiguous
                                                                  : native::Resolution::missing;
        catalog.relationships.push_back({edge.key, edge.source_key, edge.target_key, edge.candidate_keys, edge.type,
                                         edge.assignment_name, resolution});
    }
    const auto append = [&](const auto &self, const ContentNode &node,
                            const std::optional<std::string> &parent) -> void {
        const auto node_id = "node-" + std::to_string(catalog.nodes.size());
        catalog.nodes.push_back({node_id, parent,
                                 node.object_key.empty() ? std::nullopt : std::optional{node.object_key},
                                 node.node_type, node.display_name});
        for (const auto &child : node.children)
            self(self, child, node_id);
    };
    for (const auto &root : state.tree.roots)
        append(append, root, std::nullopt);
    for (const auto &issue : state.validation.issues) {
        const auto severity = issue.severity == ValidationSeverity::error     ? native::Severity::error
                              : issue.severity == ValidationSeverity::warning ? native::Severity::warning
                                                                              : native::Severity::info;
        catalog.diagnostics.push_back({issue.code, severity, issue.message,
                                       issue.object_key.empty() ? std::nullopt : std::optional{issue.object_key}});
        if (severity == native::Severity::error)
            summary.completeness = native::Completeness::invalid;
    }
    if (const auto *set = std::get_if<FloppyDiskSet>(&state.media.storage());
        set && set->status() == FloppySetStatus::incomplete && summary.completeness != native::Completeness::invalid)
        summary.completeness = native::Completeness::incomplete;
    return std::shared_ptr<const NativeDocument>{
        new NativeDocument{std::move(decoded), std::move(sources), std::move(summary), std::move(catalog)}};
}

} // namespace axk::formats::a_series
