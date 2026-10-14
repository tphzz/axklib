#include "axklib/formats/a_series/document.hpp"

#include <utility>

#include "axklib/diagnostics.hpp"

namespace axk::formats::a_series {

Document::Document(DocumentState state) : state_(std::move(state)) {}

Result<std::shared_ptr<const Document>> Document::open(MediaContainer media, const CancellationToken &cancellation,
                                                       ProgressSink *progress, diagnostic_sink *diagnostics) {
    if (auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    const auto mode = media.kind() == MediaKind::fat12_floppy_set ? MediaObjectReadMode::complete
                                                                  : MediaObjectReadMode::decoded_metadata;
    auto inventory = build_media_inventory(media, mode, 64U * 1024U * 1024U, cancellation, progress, diagnostics);
    if (!inventory)
        return std::unexpected(inventory.error());
    DiagnosticTimer graph_timer{diagnostics, diagnostic_source::objects, "image_open_phase", "relationships"};
    auto graph = build_relationship_graph(inventory->catalog);
    graph_timer.finish();
    DiagnosticTimer tree_timer{diagnostics, diagnostic_source::objects, "image_open_phase", "content-tree"};
    auto tree = build_content_tree(media, inventory->catalog, graph);
    tree_timer.finish();
    DiagnosticTimer validation_timer{diagnostics, diagnostic_source::objects, "image_open_phase", "validation"};
    auto validation = validate_semantics(media, *inventory, graph);
    validation_timer.finish();
    if (auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    std::unordered_map<std::string, const MediaObjectDescriptor *> descriptors;
    descriptors.reserve(inventory->objects.size());
    for (const auto &descriptor : inventory->objects)
        descriptors.emplace(descriptor.key, &descriptor);
    return std::shared_ptr<const Document>{
        new Document{{std::move(media), std::move(*inventory), std::move(graph), std::move(tree), std::move(validation),
                      std::move(descriptors)}}};
}

const DocumentState &Document::state() const noexcept { return state_; }

} // namespace axk::formats::a_series
