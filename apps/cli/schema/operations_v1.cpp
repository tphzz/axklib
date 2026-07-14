#include "operations_v1.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "axklib/utf8.hpp"

namespace axk::cli::schema::operations_v1 {
namespace {

using OrderedJson = nlohmann::ordered_json;

Error serialization_error(const nlohmann::json::exception &error) {
  return make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                    std::string{"could not serialize CLI JSON: "} + error.what());
}

OrderedJson audio_json(const AudioImportOutput &audio) {
  return {
      {"source_path", audio.source_path_utf8},
      {"source_format", audio.source_format},
      {"source_subtype", audio.source_subtype},
      {"source_channels", audio.source_channels},
      {"source_sample_rate", audio.source_sample_rate},
      {"output_sample_rate", audio.output_sample_rate},
      {"output_frames", audio.output_frames},
      {"resampled", audio.resampled},
      {"quantized", audio.quantized},
      {"dither_algorithm", audio.dither_algorithm},
      {"split_stereo", audio.split_stereo},
      {"clipped_samples", audio.clipped_samples},
  };
}

} // namespace

AlterationOutput project_alteration(const AlterationResult &altered) {
  AlterationOutput result{
      .source_path_utf8 = text::path_to_utf8(altered.source_path),
      .output_path_utf8 = altered.output_path
                              ? std::optional{text::path_to_utf8(*altered.output_path)}
                              : std::nullopt,
      .applied = altered.applied,
      .operations = {},
  };
  result.operations.reserve(altered.operations.size());
  for (const auto &operation : altered.operations) {
    OperationOutput projected{
        .id = operation.id,
        .type = operation.type,
        .partition_index = operation.partition.value,
        .volume_name = operation.volume_name,
        .object_name = operation.object_name,
        .removed_sfs_ids = {},
        .inserted_sfs_ids = {},
        .freed_clusters = operation.freed_clusters,
        .allocated_clusters = operation.allocated_clusters,
        .audio_import = std::nullopt,
    };
    std::ranges::transform(operation.removed_sfs_ids, std::back_inserter(projected.removed_sfs_ids),
                           [](SfsId id) { return id.value; });
    std::ranges::transform(operation.inserted_sfs_ids,
                           std::back_inserter(projected.inserted_sfs_ids),
                           [](SfsId id) { return id.value; });
    if (operation.audio_import) {
      const auto &audio = *operation.audio_import;
      projected.audio_import = AudioImportOutput{
          .source_path_utf8 = text::path_to_utf8(audio.source_path),
          .source_format = audio.source_format,
          .source_subtype = audio.source_subtype,
          .source_channels = audio.source_channels,
          .source_sample_rate = audio.source_sample_rate,
          .output_sample_rate = audio.output_sample_rate,
          .output_frames = audio.output_frames,
          .resampled = audio.resampled,
          .quantized = audio.quantized,
          .dither_algorithm = audio.dither_algorithm,
          .split_stereo = audio.split_stereo,
          .clipped_samples = audio.clipped_samples,
      };
    }
    result.operations.push_back(std::move(projected));
  }
  return result;
}

Result<std::string> serialize(const AlterationOutput &output, bool pretty) {
  try {
    auto operations = OrderedJson::array();
    for (const auto &operation : output.operations) {
      operations.push_back({
          {"id", operation.id},
          {"type", operation.type},
          {"partition_index", operation.partition_index},
          {"volume_name", operation.volume_name},
          {"object_name", operation.object_name},
          {"removed_sfs_ids", operation.removed_sfs_ids},
          {"inserted_sfs_ids", operation.inserted_sfs_ids},
          {"freed_clusters", operation.freed_clusters},
          {"allocated_clusters", operation.allocated_clusters},
          {"audio_import",
           operation.audio_import ? audio_json(*operation.audio_import) : OrderedJson(nullptr)},
      });
    }
    return OrderedJson{
        {"source_path", output.source_path_utf8},
        {"output_path",
         output.output_path_utf8 ? OrderedJson(*output.output_path_utf8) : OrderedJson(nullptr)},
        {"applied", output.applied},
        {"operations", std::move(operations)},
    }
        .dump(pretty ? 2 : -1);
  } catch (const nlohmann::json::exception &error) {
    return std::unexpected{serialization_error(error)};
  }
}

} // namespace axk::cli::schema::operations_v1
