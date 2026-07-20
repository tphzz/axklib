#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "axklib/audio_export.hpp"
#include "axklib/error.hpp"
#include "axklib/relationship.hpp"

namespace axk::app {

inline constexpr std::string_view volume_graph_schema_version{"axklib.volume_graph.v2"};
inline constexpr std::string_view unresolved_wave_data_schema_version{"axklib.unresolved_wave_data.v1"};

axk::Result<std::string> serialize_volume_graph(const VolumeExport &volume, const RelationshipGraph &graph,
                                                const std::filesystem::path &source_path,
                                                std::string_view container_kind = "sfs");
axk::Result<std::string> serialize_unresolved_wave_data_graph(const UnresolvedWaveDataExport &scope,
                                                              const std::filesystem::path &source_path,
                                                              std::string_view container_kind = "sfs");

} // namespace axk::app
