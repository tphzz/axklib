#include "info_v1.hpp"

#include <nlohmann/json.hpp>

namespace axk::cli::schema::info_v1 {
namespace {

using OrderedJson = nlohmann::ordered_json;

OrderedJson node_json(const NodeOutput& node) {
  auto children = OrderedJson::array();
  for (const auto& child : node.children)
    children.push_back(node_json(child));
  return {
      {"node_id", node.node_id},
      {"node_type", node.node_type},
      {"display_name", node.display_name},
      {"object_key", node.object_key},
      {"object_type", node.object_type},
      {"count", node.count ? OrderedJson(*node.count) : OrderedJson(nullptr)},
      {"details", node.details},
      {"quality", node.quality},
      {"basis", node.basis},
      {"notes", node.notes},
      {"selector_path", node.selector_path},
      {"children", std::move(children)},
  };
}

}  // namespace

Result<std::string> serialize(const InfoOutput& output) {
  try {
    auto trees = OrderedJson::array();
    for (const auto& tree : output.trees) {
      auto roots = OrderedJson::array();
      for (const auto& root : tree.roots)
        roots.push_back(node_json(root));
      trees.push_back({
          {"source_path", tree.source_path_utf8},
          {"container_kind", tree.container_kind},
          {"detected_format", tree.detected_format},
          {"roots", std::move(roots)},
          {"issues", OrderedJson::array()},
      });
    }
    auto errors = OrderedJson::array();
    for (const auto& error : output.load_errors) {
      errors.push_back({
          {"path", error.path_utf8},
          {"error_code", error.error_code},
          {"message", error.message},
          {"original_exception", error.original_exception},
      });
    }
    return OrderedJson{{"trees", std::move(trees)}, {"load_errors", std::move(errors)}}.dump(2);
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                                      std::string{"could not serialize info JSON: "} +
                                          error.what())};
  }
}

}  // namespace axk::cli::schema::info_v1
