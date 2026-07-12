#include "semantic_v1.hpp"

#include <nlohmann/json.hpp>

namespace axk::cli::schema::semantic_v1 {
namespace {

using OrderedJson = nlohmann::ordered_json;

TreeNodeOutput project_node(const ContentNode &node) {
  TreeNodeOutput result{
      .node_id = node.node_id,
      .node_type = node.node_type,
      .display_name = node.display_name,
      .object_key = node.object_key,
      .object_type = node.object_type,
      .quality = std::string{relationship_quality_name(node.quality)},
      .basis = node.basis,
      .notes = node.notes,
      .details = node.details,
      .children = {},
  };
  for (const auto &child : node.children)
    result.children.push_back(project_node(child));
  return result;
}

OrderedJson node_json(const TreeNodeOutput &node) {
  auto children = OrderedJson::array();
  for (const auto &child : node.children)
    children.push_back(node_json(child));
  return {
      {"node_id", node.node_id},
      {"node_type", node.node_type},
      {"display_name", node.display_name},
      {"object_key", node.object_key},
      {"object_type", node.object_type},
      {"quality", node.quality},
      {"basis", node.basis},
      {"notes", node.notes},
      {"details", node.details},
      {"children", std::move(children)},
  };
}

Error serialization_error(const nlohmann::json::exception &error) {
  return make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                    std::string{"could not serialize semantic JSON: "} + error.what());
}

} // namespace

RelationshipsOutput project_relationships(const RelationshipGraph &graph) {
  RelationshipsOutput result;
  for (const auto &row : graph.relationships) {
    result.relationships.push_back({
        .key = row.key,
        .source_key = row.source_key,
        .target_key = row.target_key,
        .candidate_keys = row.candidate_keys,
        .relationship_type = row.type,
        .quality = std::string{relationship_quality_name(row.quality)},
        .basis = row.basis,
        .notes = row.notes,
        .scope_key = row.scope_key,
        .assignment_index = row.assignment_index,
        .assignment_name = row.assignment_name,
        .active_assignment_state = std::string{assignment_state_name(row.assignment_state)},
        .assignment_rch_assign_display = row.receive_channel_display,
    });
  }
  for (const auto &row : graph.bitmap_comparisons) {
    result.bitmap_comparisons.push_back({
        .sbnk_key = row.sbnk_key,
        .bitmap_programs = row.bitmap_programs,
        .direct_assignment_programs = row.direct_assignment_programs,
        .indirect_assignment_programs = row.indirect_assignment_programs,
        .bitmap_without_direct = row.bitmap_without_direct,
        .direct_without_bitmap = row.direct_without_bitmap,
        .status = row.status,
        .mismatch_class = row.mismatch_class,
    });
  }
  return result;
}

TreeOutput project_tree(const ContentTree &tree) {
  TreeOutput result{.source_path_utf8 = tree.source_path, .roots = {}};
  for (const auto &root : tree.roots)
    result.roots.push_back(project_node(root));
  return result;
}

Result<std::string> serialize(const RelationshipsOutput &output, bool pretty) {
  try {
    auto relationships = OrderedJson::array();
    for (const auto &row : output.relationships) {
      relationships.push_back({
          {"key", row.key},
          {"source_key", row.source_key},
          {"target_key", row.target_key ? OrderedJson(*row.target_key) : OrderedJson(nullptr)},
          {"candidate_keys", row.candidate_keys},
          {"relationship_type", row.relationship_type},
          {"quality", row.quality},
          {"basis", row.basis},
          {"notes", row.notes},
          {"scope_key", row.scope_key},
          {"assignment_index",
           row.assignment_index ? OrderedJson(*row.assignment_index) : OrderedJson(nullptr)},
          {"assignment_name", row.assignment_name},
          {"active_assignment_state", row.active_assignment_state},
          {"assignment_rch_assign_display", row.assignment_rch_assign_display},
      });
    }
    auto bitmaps = OrderedJson::array();
    for (const auto &row : output.bitmap_comparisons) {
      bitmaps.push_back({
          {"sbnk_key", row.sbnk_key},
          {"bitmap_programs", row.bitmap_programs},
          {"direct_assignment_programs", row.direct_assignment_programs},
          {"indirect_assignment_programs", row.indirect_assignment_programs},
          {"bitmap_without_direct", row.bitmap_without_direct},
          {"direct_without_bitmap", row.direct_without_bitmap},
          {"status", row.status},
          {"mismatch_class", row.mismatch_class},
      });
    }
    return OrderedJson{
        {"schema_version", schema_version},
        {"relationships", std::move(relationships)},
        {"bitmap_comparisons", std::move(bitmaps)},
    }
        .dump(pretty ? 2 : -1);
  } catch (const nlohmann::json::exception &error) {
    return std::unexpected{serialization_error(error)};
  }
}

Result<std::string> serialize(const TreeOutput &output, bool pretty) {
  try {
    auto roots = OrderedJson::array();
    for (const auto &root : output.roots)
      roots.push_back(node_json(root));
    return OrderedJson{{"schema_version", schema_version},
                       {"source_path", output.source_path_utf8},
                       {"roots", std::move(roots)}}
        .dump(pretty ? 2 : -1);
  } catch (const nlohmann::json::exception &error) {
    return std::unexpected{serialization_error(error)};
  }
}

Result<std::string> serialize(const std::vector<WaveformOutput> &output, bool pretty) {
  try {
    auto waveforms = OrderedJson::array();
    for (const auto &row : output) {
      waveforms.push_back({
          {"partition_index", row.partition_index},
          {"sfs_id", row.sfs_id},
          {"object_key", row.object_key},
          {"name", row.name},
          {"wav_path", row.wav_path_utf8},
          {"sample_rate", row.sample_rate},
          {"sample_width_bytes", row.sample_width_bytes},
          {"stored_sample_width_bytes", row.stored_sample_width_bytes},
          {"frame_count", row.frame_count},
          {"stored_payload_size", row.stored_payload_size},
          {"stored_payload_transform", row.stored_payload_transform},
          {"alternating_byte_payload_detected", row.alternating_byte_payload_detected},
      });
    }
    return OrderedJson{{"schema_version", schema_version}, {"waveforms", std::move(waveforms)}}
        .dump(pretty ? 2 : -1);
  } catch (const nlohmann::json::exception &error) {
    return std::unexpected{serialization_error(error)};
  }
}

} // namespace axk::cli::schema::semantic_v1
