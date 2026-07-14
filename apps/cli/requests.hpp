#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace axk::cli {

struct InfoRequest {
  std::vector<std::filesystem::path> paths;
  bool strict{};
  std::string format{"tree"};
  std::optional<std::size_t> max_depth;
  bool show_quality{};
  bool show_unresolved{};
  bool show_default_programs{};
};

struct InventoryRequest {
  std::vector<std::filesystem::path> paths;
  std::filesystem::path output_directory;
  bool strict{};
  bool overwrite{};
};

struct ValidateRequest {
  std::vector<std::filesystem::path> paths;
  std::optional<std::filesystem::path> exports;
  std::filesystem::path output_directory;
  std::string policy{"normal"};
  bool strict{};
  bool overwrite{};
};

struct RelationshipsRequest {
  std::vector<std::filesystem::path> paths;
  std::filesystem::path output_directory;
  bool overwrite{};
  std::optional<std::filesystem::path> mono_directory;
};

struct CoverageRequest {
  std::vector<std::filesystem::path> paths;
  std::filesystem::path output_directory;
  bool overwrite{};
};

struct CreateHdsRequest {
  std::filesystem::path manifest;
  std::filesystem::path output;
  bool overwrite{};
};

struct AlterHdsRequest {
  std::filesystem::path source;
  std::filesystem::path transaction;
  std::optional<std::filesystem::path> output;
};

struct OrphansRequest {
  std::vector<std::filesystem::path> paths;
  std::filesystem::path output_directory;
  bool overwrite{};
};

struct ObjectsRequest {
  std::vector<std::filesystem::path> paths;
  std::optional<std::filesystem::path> output_directory;
  std::optional<std::string> object_type;
  bool with_payloads{};
  bool strict{};
  bool overwrite{};
  bool pretty{};
};

struct CorpusAuditRequest {
  std::vector<std::filesystem::path> paths;
  std::filesystem::path output_directory;
  std::string policy{"normal"};
  std::size_t wave_smoke_limit{10};
  bool skip_wave_smoke{};
  bool overwrite{};
};

struct ExtractRequest {
  std::string scope;
  std::vector<std::filesystem::path> paths;
  std::filesystem::path output_directory;
  std::vector<std::string> selector_paths;
  std::string stereo{"auto"};
  std::string progress{"auto"};
  bool overwrite{};
  bool strict{};
  bool sfz{};
};

struct PackageExportRequest {
  std::filesystem::path source;
  std::vector<std::string> roots;
  std::optional<std::uint32_t> partition_index;
  std::string group_name;
  std::string volume_name;
  std::filesystem::path output;
  std::string format{"summary"};
  bool overwrite{};
};

struct PackageReadRequest {
  std::filesystem::path package;
  std::string format{"summary"};
};

struct PackageImportRequest {
  std::filesystem::path target;
  std::vector<std::filesystem::path> packages;
  std::vector<std::string> destinations;
  std::optional<std::filesystem::path> rename_map;
  std::optional<std::filesystem::path> output;
  std::string reuse_scope{"volume"};
  std::string format{"summary"};
  bool overwrite{};
  bool apply{};
};

} // namespace axk::cli
