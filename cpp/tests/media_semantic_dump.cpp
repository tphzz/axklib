#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

namespace {

std::string_view object_type(axk::ObjectType type) {
  switch (type) {
  case axk::ObjectType::smpl:
    return "SMPL";
  case axk::ObjectType::sbnk:
    return "SBNK";
  case axk::ObjectType::sbac:
    return "SBAC";
  case axk::ObjectType::prog:
    return "PROG";
  case axk::ObjectType::sequ:
    return "SEQU";
  case axk::ObjectType::prf3:
    return "PRF3";
  case axk::ObjectType::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

void append_tree_rows(nlohmann::ordered_json &rows, const axk::ContentNode &node) {
  rows.push_back({node.node_type, node.display_name, node.object_type});
  for (const auto &child : node.children)
    append_tree_rows(rows, child);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto opened = axk::open_media(std::filesystem::path{argv[1]});
  if (!opened) {
    std::cerr << axk::render_error(opened.error()) << '\n';
    return 1;
  }
  const auto media_objects = opened->objects();
  if (!media_objects) {
    std::cerr << axk::render_error(media_objects.error()) << '\n';
    return 1;
  }
  const auto catalog = axk::build_object_catalog(*opened);
  if (!catalog) {
    std::cerr << axk::render_error(catalog.error()) << '\n';
    return 1;
  }
  const auto graph = axk::build_relationship_graph(*catalog);
  const auto tree = axk::build_content_tree(*opened, *catalog, graph);

  std::map<std::string, const axk::MediaObject *> media_by_key;
  for (const auto &object : *media_objects)
    media_by_key.emplace(object.key, &object);
  std::map<std::string, const axk::ObjectSnapshot *> catalog_by_key;
  for (const auto &object : catalog->objects)
    catalog_by_key.emplace(object.key, &object);

  std::vector<std::tuple<std::string, std::string, std::uint64_t, std::uint64_t>> object_rows;
  for (const auto &object : catalog->objects) {
    const auto media = media_by_key.find(object.key);
    const auto offset = media == media_by_key.end() ? 0U : media->second->data_offset;
    const auto size = media == media_by_key.end()
                          ? static_cast<std::uint64_t>(object.object.header.header_size) +
                                object.object.header.payload_bytes_0x1c
                          : static_cast<std::uint64_t>(media->second->raw_payload.size());
    object_rows.emplace_back(object_type(object.object.header.type), object.object.header.name,
                             offset, size);
  }
  std::ranges::sort(object_rows);

  std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string>>
      relationship_rows;
  for (const auto &relationship : graph.relationships) {
    const auto source_entry = catalog_by_key.find(relationship.source_key);
    if (source_entry == catalog_by_key.end())
      continue;
    const auto source = source_entry->second;
    std::string target_identity{":"};
    if (relationship.target_key) {
      const auto target_entry = catalog_by_key.find(*relationship.target_key);
      if (target_entry != catalog_by_key.end()) {
        const auto target = target_entry->second;
        target_identity = std::format("{}:{}", object_type(target->object.header.type),
                                      target->object.header.name);
      }
    }
    relationship_rows.emplace_back(
        std::format("{}:{}", object_type(source->object.header.type), source->object.header.name),
        std::move(target_identity), relationship.type,
        std::string{axk::relationship_quality_name(relationship.quality)}, relationship.basis);
  }
  std::ranges::sort(relationship_rows);

  nlohmann::ordered_json trees = nlohmann::ordered_json::array();
  for (const auto &root : tree.roots)
    append_tree_rows(trees, root);
  std::vector<std::tuple<std::string, std::string, std::string>> tree_rows;
  for (const auto &row : trees)
    tree_rows.emplace_back(row[0].get<std::string>(), row[1].get<std::string>(),
                           row[2].get<std::string>());
  std::ranges::sort(tree_rows);

  std::cout << nlohmann::ordered_json{{"objects", object_rows},
                                      {"relationships", relationship_rows},
                                      {"tree", tree_rows}}
                   .dump()
            << '\n';
  return 0;
}
