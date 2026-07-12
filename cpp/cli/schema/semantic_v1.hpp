#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

namespace axk::cli::schema::semantic_v1 {

inline constexpr std::string_view schema_version{"1.0"};

struct RelationshipOutput {
  std::string key;
  std::string source_key;
  std::optional<std::string> target_key;
  std::vector<std::string> candidate_keys;
  std::string relationship_type;
  std::string quality;
  std::string basis;
  std::string notes;
  std::string scope_key;
  std::optional<std::size_t> assignment_index;
  std::string assignment_name;
  std::string active_assignment_state;
  std::string assignment_rch_assign_display;
};

struct BitmapOutput {
  std::string sbnk_key;
  std::vector<std::uint8_t> bitmap_programs;
  std::vector<std::uint8_t> direct_assignment_programs;
  std::vector<std::uint8_t> indirect_assignment_programs;
  std::vector<std::uint8_t> bitmap_without_direct;
  std::vector<std::uint8_t> direct_without_bitmap;
  std::string status;
  std::string mismatch_class;
};

struct RelationshipsOutput {
  std::vector<RelationshipOutput> relationships;
  std::vector<BitmapOutput> bitmap_comparisons;
};

struct TreeNodeOutput {
  std::string node_id;
  std::string node_type;
  std::string display_name;
  std::string object_key;
  std::string object_type;
  std::string quality;
  std::string basis;
  std::string notes;
  std::vector<std::string> details;
  std::vector<TreeNodeOutput> children;
};

struct TreeOutput {
  std::string source_path_utf8;
  std::vector<TreeNodeOutput> roots;
};

struct WaveformOutput {
  std::uint8_t partition_index{};
  std::uint32_t sfs_id{};
  std::string object_key;
  std::string name;
  std::string wav_path_utf8;
  std::uint32_t sample_rate{};
  std::uint16_t sample_width_bytes{};
  std::uint16_t stored_sample_width_bytes{};
  std::uint64_t frame_count{};
  std::uint64_t stored_payload_size{};
  std::string stored_payload_transform;
  bool alternating_byte_payload_detected{};
};

RelationshipsOutput project_relationships(const RelationshipGraph& graph);
TreeOutput project_tree(const ContentTree& tree);
Result<std::string> serialize(const RelationshipsOutput& output, bool pretty);
Result<std::string> serialize(const TreeOutput& output, bool pretty);
Result<std::string> serialize(const std::vector<WaveformOutput>& output, bool pretty);

}  // namespace axk::cli::schema::semantic_v1
