#include "export_v1.hpp"

#include "axklib/application/volume_graph.hpp"

axk::Result<std::string> axk::cli::schema::export_v1::serialize_volume_graph(const VolumeExport &volume,
                                                                             const RelationshipGraph &graph,
                                                                             const std::filesystem::path &source_path,
                                                                             std::string_view container_kind) {
    return app::serialize_volume_graph(volume, graph, source_path, container_kind);
}
