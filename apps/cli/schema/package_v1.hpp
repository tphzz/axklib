#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/package.hpp"

namespace axk::cli::schema::package_v1 {

inline constexpr std::string_view schema_version{"1.0"};

struct RootOutput {
  std::string kind;
  std::string display_name;
  std::vector<std::string> node_ids;
};

struct NodeOutput {
  std::string node_id;
  std::string object_type;
  std::string name;
  std::string payload_sha256;
  std::string normalized_sha256;
  std::optional<std::string> semantic_sha256;
  std::optional<std::string> audio_sha256;
};

struct IssueOutput {
  std::string code;
  std::string message;
  bool fatal{};
};

struct PackageOutput {
  std::string path_utf8;
  std::string package_id;
  std::string package_kind;
  std::string required_extension;
  std::string source_media_kind;
  bool valid{};
  bool payloads_verified{};
  std::uint64_t relationship_count{};
  std::vector<RootOutput> roots;
  std::vector<NodeOutput> objects;
  std::vector<IssueOutput> issues;
};

struct ConflictOutput {
  std::string code;
  std::string message;
  std::optional<std::uint64_t> package_index;
  std::optional<std::uint64_t> root_index;
  std::string package_id;
  std::string node_id;
  std::optional<std::uint32_t> partition_index;
  std::string group_name;
  std::string volume_name;
  std::string raw_group;
  std::string raw_volume;
};

struct ActionOutput {
  std::string action_id;
  std::uint64_t package_index{};
  std::uint64_t root_index{};
  std::string package_id;
  std::string node_id;
  std::string object_type;
  std::string source_name;
  std::string destination_name;
  std::uint32_t partition_index{};
  std::string group_name;
  std::string volume_name;
  std::string raw_group;
  std::string raw_volume;
  std::vector<std::string> actions;
  std::optional<std::string> canonical_action_id;
  std::optional<std::uint32_t> target_sfs_id;
  std::optional<std::uint32_t> target_link_id;
};

struct AllocationOutput {
  std::uint32_t partition_index{};
  std::string group_name;
  std::string volume_name;
  std::string raw_group;
  std::string raw_volume;
  std::uint64_t inserted_object_count{};
  std::uint64_t reused_object_count{};
  std::uint64_t payload_clusters{};
  std::uint64_t payload_sectors{};
  std::uint64_t continuation_clusters{};
  std::uint64_t directory_growth_bytes{};
  std::uint64_t remaining_object_ids{};
  std::uint64_t remaining_clusters{};
  std::uint64_t projected_image_sectors{};
  std::uint64_t projected_image_size_bytes{};
};

struct ImportResultOutput {
  std::string output_path_utf8;
  std::string source_snapshot_id;
  std::string output_snapshot_id;
  bool applied{};
};

struct PlanOutput {
  std::string target_path_utf8;
  std::vector<std::string> package_paths_utf8;
  std::string plan_id;
  std::string target_kind;
  std::string target_snapshot_id;
  bool valid{};
  std::vector<IssueOutput> warnings;
  std::vector<ConflictOutput> conflicts;
  std::vector<ActionOutput> objects;
  std::vector<AllocationOutput> allocation;
  std::optional<ImportResultOutput> result;
};

PackageOutput project_package(const std::filesystem::path &path, const PortablePackage &package);
PlanOutput project_plan(const std::filesystem::path &target,
                        const std::vector<std::filesystem::path> &package_paths,
                        const PackageImportPlan &plan,
                        const std::optional<PackageImportReport> &report = std::nullopt);
Result<std::string> serialize(const PackageOutput &output, bool pretty);
Result<std::string> serialize(const PlanOutput &output, bool pretty);

} // namespace axk::cli::schema::package_v1
