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

inline constexpr std::string_view schema_version{"1.0"};
inline constexpr std::string_view volume_graph_schema_version{"axklib.volume_graph.v1"};

struct VolumeSummaryOutput {
  std::string path_utf8;
  std::string graph_path_utf8;
  std::uint64_t waveform_count{};
  std::uint64_t sample_bank_count{};
};

Result<std::string> serialize_volume_graph(const VolumeExport &volume,
                                           const RelationshipGraph &graph,
                                           const std::filesystem::path &source_path);
Result<std::string> serialize(const std::vector<VolumeSummaryOutput> &volumes, bool pretty);

} // namespace axk::cli::schema::export_v1
