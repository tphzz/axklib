#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/error.hpp"

namespace axk::cli::schema::operations_v1 {

inline constexpr std::string_view alteration_schema_version{"compat-v1"};

struct AudioImportOutput {
  std::string source_path_utf8;
  std::string source_format;
  std::string source_subtype;
  std::uint8_t source_channels{};
  std::uint32_t source_sample_rate{};
  std::uint32_t output_sample_rate{};
  std::uint64_t output_frames{};
  bool resampled{};
  bool quantized{};
  std::string dither_algorithm;
  bool split_stereo{};
  std::uint64_t clipped_samples{};
};

struct OperationOutput {
  std::string id;
  std::string type;
  std::uint8_t partition_index{};
  std::string volume_name;
  std::string object_name;
  std::vector<std::uint32_t> removed_sfs_ids;
  std::vector<std::uint32_t> inserted_sfs_ids;
  std::uint64_t freed_clusters{};
  std::uint64_t allocated_clusters{};
  std::optional<AudioImportOutput> audio_import;
};

struct AlterationOutput {
  std::string source_path_utf8;
  std::optional<std::string> output_path_utf8;
  bool applied{};
  std::vector<OperationOutput> operations;
};

AlterationOutput project_alteration(const AlterationResult &altered);
Result<std::string> serialize(const AlterationOutput &output, bool pretty);

} // namespace axk::cli::schema::operations_v1
