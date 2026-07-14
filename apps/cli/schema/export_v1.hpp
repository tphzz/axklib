#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/audio_export.hpp"
#include "axklib/error.hpp"
#include "axklib/relationship.hpp"

namespace axk::cli::schema::export_v1 {

inline constexpr std::string_view volume_graph_schema_version{
    "axklib.volume_graph.v1"};

Result<std::string>
serialize_volume_graph(const VolumeExport &volume,
                       const RelationshipGraph &graph,
                       const std::filesystem::path &source_path,
                       std::string_view container_kind = "sfs");

} // namespace axk::cli::schema::export_v1
