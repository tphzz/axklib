#include "axklib/semantic.hpp"

#include "axklib/media.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>

namespace axk {
namespace {

std::string object_type_name(ObjectType type) {
  switch (type) {
  case ObjectType::smpl:
    return "SMPL";
  case ObjectType::sbnk:
    return "SBNK";
  case ObjectType::sbac:
    return "SBAC";
  case ObjectType::prog:
    return "PROG";
  case ObjectType::sequ:
    return "SEQU";
  case ObjectType::prf3:
    return "PRF3";
  case ObjectType::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::string lowercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string display_text(std::string value) {
  const auto visible = [](unsigned char ch) { return std::isspace(ch) == 0; };
  const auto first = std::ranges::find_if(value, visible);
  const auto last = std::ranges::find_if(value | std::views::reverse, visible).base();
  if (first >= last)
    return {};
  return {first, last};
}

const ObjectSnapshot *find_object(const ObjectCatalog &catalog, std::string_view key) {
  const auto found = std::ranges::find(catalog.objects, key, &ObjectSnapshot::key);
  return found == catalog.objects.end() ? nullptr : &*found;
}

std::optional<unsigned int> program_slot(const ObjectSnapshot &item) {
  if (item.object.header.type != ObjectType::prog)
    return std::nullopt;
  unsigned int value{};
  const auto &name = item.object.header.name;
  const auto parsed = std::from_chars(name.data(), name.data() + name.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || value < 1U ||
      value > 128U) {
    return std::nullopt;
  }
  return value;
}

std::string display_program(const ObjectSnapshot &item) {
  const auto slot = program_slot(item);
  if (!slot)
    return item.object.header.name;
  if (const auto *program = std::get_if<CurrentProg>(&item.object.payload);
      program != nullptr && !program->program_name.empty()) {
    return std::format("{:03}: {}", *slot, program->program_name);
  }
  return std::format("{:03}: Pgm {:03}", *slot, *slot);
}

bool navigable(const Relationship &row) {
  if (row.type.starts_with("PROG_ASSIGNMENT_TO_")) {
    return (row.assignment_state == AssignmentState::active ||
            row.assignment_state == AssignmentState::source_load) &&
           (row.quality == RelationshipQuality::known ||
            row.quality == RelationshipQuality::likely) &&
           row.target_key.has_value();
  }
  return row.quality == RelationshipQuality::known && row.target_key.has_value();
}

ContentNode bank_node(const ObjectSnapshot &item, const Relationship *row = nullptr) {
  ContentNode result{
      std::format("object:{}", item.key),
      "sample_bank",
      display_text(item.object.header.name),
      item.key,
      "SBNK",
  };
  if (row != nullptr) {
    result.quality = row->quality;
    result.basis = row->basis;
    result.notes = row->notes;
    if ((row->assignment_state == AssignmentState::active ||
         row->assignment_state == AssignmentState::source_load) &&
        !row->receive_channel_display.empty() && row->receive_channel_display != "off" &&
        row->receive_channel_display != "unknown") {
      result.details.push_back(std::format("Rch Assign: {}", row->receive_channel_display));
    }
  } else {
    result.basis = "container object metadata";
  }
  return result;
}

ContentNode group_node(const ObjectSnapshot &item, const ObjectCatalog &catalog,
                       const RelationshipGraph &graph, const Relationship *parent = nullptr,
                       bool with_children = true) {
  ContentNode result{
      std::format("object:{}", item.key),
      "sample_bank",
      std::format("B {}", display_text(item.object.header.name)),
      item.key,
      "SBAC",
  };
  result.basis = parent == nullptr ? "current SBAC slot relationships" : parent->basis;
  if (parent != nullptr)
    result.quality = parent->quality;
  if (parent != nullptr &&
      (parent->assignment_state == AssignmentState::active ||
       parent->assignment_state == AssignmentState::source_load) &&
      !parent->receive_channel_display.empty() && parent->receive_channel_display != "off" &&
      parent->receive_channel_display != "unknown") {
    result.details.push_back(std::format("Rch Assign: {}", parent->receive_channel_display));
  }
  if (with_children) {
    for (const auto *row : graph.children(item.key)) {
      if (row->type != "SBAC_SLOT_TO_SBNK" || !navigable(*row))
        continue;
      if (const auto *child = find_object(catalog, *row->target_key);
          child != nullptr && child->object.header.type == ObjectType::sbnk) {
        result.children.push_back(bank_node(*child, row));
      }
    }
  }
  return result;
}

std::string sampler_path(const ObjectSnapshot &item) {
  if (!item.placement)
    return std::format("partition {}", item.partition.value);
  return std::format("partition {}: {}/{}", item.partition.value, item.placement->partition_name,
                     item.placement->volume_name);
}

bool partition_has_unknown_records(const Container &container, PartitionIndex index) {
  const auto partition = std::ranges::find(container.partitions(), index.value,
                                           [](const Partition &p) { return p.index.value; });
  if (partition == container.partitions().end())
    return true;
  return std::ranges::any_of(partition->records, [](const IndexRecord &record) {
    return record.sfs_id.value != 0U && record.payload_kind == PayloadKind::unknown;
  });
}

std::string join(const std::vector<std::string> &values) {
  std::string result;
  for (const auto &value : values) {
    if (!result.empty())
      result += " | ";
    result += value;
  }
  return result;
}

} // namespace

static ContentTree
build_content_tree_impl(std::string source_path,
                        const std::vector<std::pair<PartitionIndex, std::string>> &partitions,
                        const ObjectCatalog &catalog, const RelationshipGraph &graph,
                        bool include_default_programs, bool prefix_partition_index) {
  ContentTree result;
  result.source_path = std::move(source_path);

  using VolumeKey = std::tuple<std::uint8_t, std::uint32_t, std::string, std::string>;
  using PartitionKey = std::pair<std::uint8_t, std::string>;
  std::map<VolumeKey, std::vector<const ObjectSnapshot *>> volumes;
  for (const auto &item : catalog.objects) {
    if (!item.placement)
      continue;
    volumes[{item.partition.value, item.placement->volume_directory.value,
             item.placement->partition_name, item.placement->volume_name}]
        .push_back(&item);
  }

  std::map<PartitionKey, std::vector<ContentNode>> partition_volumes;
  std::map<std::pair<std::string, std::string>, std::size_t> duplicate_volume_names;
  for (const auto &[key, items] : volumes) {
    static_cast<void>(items);
    const auto &[partition_index, volume_id, partition_name, volume_name] = key;
    static_cast<void>(partition_index);
    static_cast<void>(volume_id);
    ++duplicate_volume_names[{partition_name, volume_name}];
  }
  for (const auto &[key, items] : volumes) {
    const auto &[partition_index, volume_id, partition_name, volume_name] = key;
    static_cast<void>(volume_id);
    auto display_volume_name = display_text(volume_name);
    if (display_volume_name.empty())
      display_volume_name = "<unnamed volume>";
    if (duplicate_volume_names[{partition_name, volume_name}] > 1U && !items.empty() &&
        items.front()->placement && !items.front()->placement->container_directory.empty()) {
      const auto &raw_path = items.front()->placement->container_directory;
      const auto separator = raw_path.find_last_of("/\\");
      const auto raw_volume = raw_path.substr(separator == std::string::npos ? 0U : separator + 1U);
      if (!raw_volume.empty())
        display_volume_name = std::format("{} ({})", display_volume_name, raw_volume);
    }
    ContentNode volume{std::format("volume:{}:{}", partition_index, volume_id), "volume",
                       std::move(display_volume_name)};

    std::vector<ContentNode> programs;
    std::vector<ContentNode> banks;
    std::vector<ContentNode> waveforms;
    std::vector<ContentNode> sequences;
    for (const auto *item : items) {
      if (item->object.header.type == ObjectType::prog) {
        ContentNode node{
            std::format("object:{}", item->key),
            "program",
            display_program(*item),
            item->key,
            "PROG",
        };
        node.basis = "container object metadata";
        for (const auto *row : graph.children(item->key)) {
          if (!row->type.starts_with("PROG_ASSIGNMENT_TO_") || !navigable(*row))
            continue;
          const auto *target = find_object(catalog, *row->target_key);
          if (target == nullptr)
            continue;
          if (target->object.header.type == ObjectType::sbac) {
            node.children.push_back(group_node(*target, catalog, graph, row, false));
          } else if (target->object.header.type == ObjectType::sbnk) {
            node.children.push_back(bank_node(*target, row));
          } else if (target->object.header.type == ObjectType::smpl) {
            ContentNode child{std::format("object:{}", target->key),
                              "waveform",
                              display_text(target->object.header.name),
                              target->key,
                              "SMPL",
                              row->quality,
                              row->basis,
                              row->notes};
            if (!row->receive_channel_display.empty() && row->receive_channel_display != "off" &&
                row->receive_channel_display != "unknown") {
              child.details.push_back(std::format("Rch Assign: {}", row->receive_channel_display));
            }
            node.children.push_back(std::move(child));
          }
        }
        const auto slot = program_slot(*item);
        const bool quiet_default = slot && node.children.empty() && node.notes.empty() &&
                                   node.quality == RelationshipQuality::known &&
                                   node.display_name == std::format("{:03}: Pgm {:03}", *slot, *slot);
        if (include_default_programs || !quiet_default)
          programs.push_back(std::move(node));
      } else if (item->object.header.type == ObjectType::sbac) {
        banks.push_back(group_node(*item, catalog, graph));
      } else if (item->object.header.type == ObjectType::sbnk) {
        const bool has_group =
            std::ranges::any_of(graph.parents(item->key), [](const Relationship *row) {
              return row->type == "SBAC_SLOT_TO_SBNK" &&
                     row->quality == RelationshipQuality::known && row->target_key.has_value();
            });
        if (!has_group)
          banks.push_back(bank_node(*item));
      } else if (item->object.header.type == ObjectType::smpl) {
        waveforms.push_back({
            std::format("object:{}", item->key),
            "waveform",
            display_text(item->object.header.name),
            item->key,
            "SMPL",
            RelationshipQuality::known,
            "container object metadata",
        });
      } else if (item->object.header.type == ObjectType::sequ) {
        sequences.push_back({
            std::format("object:{}", item->key),
            "sequence",
            display_text(item->object.header.name),
            item->key,
            "SEQU",
            RelationshipQuality::known,
            "container object metadata",
        });
      }
    }
    std::map<unsigned int, ContentNode> programs_by_slot;
    std::vector<ContentNode> unslotted_programs;
    std::ranges::sort(programs, {}, [](const ContentNode &node) {
      return std::tuple{lowercase(node.display_name), node.object_type, node.object_key};
    });
    for (auto &program : programs) {
      unsigned int slot{};
      const auto end = program.display_name.find(':');
      const auto parsed = end == 3U
                              ? std::from_chars(program.display_name.data(),
                                                program.display_name.data() + end, slot)
                              : std::from_chars_result{};
      if (end == 3U && parsed.ec == std::errc{} && slot >= 1U && slot <= 128U)
        programs_by_slot[slot] = std::move(program);
      else
        unslotted_programs.push_back(std::move(program));
    }
    programs.clear();
    for (auto &[slot, program] : programs_by_slot) {
      static_cast<void>(slot);
      programs.push_back(std::move(program));
    }
    std::ranges::move(unslotted_programs, std::back_inserter(programs));
    if (include_default_programs) {
      std::set<unsigned int> present;
      for (const auto *item : items) {
        if (const auto slot = program_slot(*item))
          present.insert(*slot);
      }
      for (unsigned int slot = 1; slot <= 128; ++slot) {
        if (present.contains(slot))
          continue;
        programs.push_back({
            std::format("default-program:{}:{}", partition_index, slot),
            "program",
            std::format("{:03}: Pgm {:03}", slot, slot),
            {},
            "PROG",
            RelationshipQuality::known,
            "synthesized empty program slot",
        });
      }
    }
    const auto sort_nodes = [](std::vector<ContentNode> &nodes) {
      std::ranges::sort(nodes, {}, [](const ContentNode &node) {
        return std::tuple{lowercase(node.display_name), node.object_type, node.object_key};
      });
    };
    sort_nodes(programs);
    sort_nodes(banks);
    sort_nodes(waveforms);
    sort_nodes(sequences);
    if (!programs.empty()) {
      volume.children.push_back({"category:Programs",
                                 "category",
                                 "Programs",
                                 {},
                                 {},
                                 RelationshipQuality::known,
                                 {},
                                 {},
                                 {},
                                 std::move(programs)});
    }
    if (!banks.empty()) {
      volume.children.push_back({"category:Sample Banks",
                                 "category",
                                 "Sample Banks",
                                 {},
                                 {},
                                 RelationshipQuality::known,
                                 {},
                                 {},
                                 {},
                                 std::move(banks)});
    }
    if (!waveforms.empty()) {
      volume.children.push_back({"category:Waveforms",
                                 "category",
                                 "Waveforms",
                                 {},
                                 {},
                                 RelationshipQuality::known,
                                 {},
                                 {},
                                 {},
                                 std::move(waveforms)});
    }
    if (!sequences.empty()) {
      volume.children.push_back({"category:Sequences",
                                 "category",
                                 "Sequences",
                                 {},
                                 {},
                                 RelationshipQuality::known,
                                 {},
                                 {},
                                 {},
                                 std::move(sequences)});
    }
    partition_volumes[{partition_index, partition_name}].push_back(std::move(volume));
  }

  std::map<std::uint8_t, std::size_t> partition_index_counts;
  for (const auto &[partition_index, partition_name] : partitions) {
    static_cast<void>(partition_name);
    ++partition_index_counts[partition_index.value];
  }
  for (const auto &[partition_index, partition_name] : partitions) {
    const auto duplicate_index = partition_index_counts[partition_index.value] > 1U;
    ContentNode root{
        duplicate_index ? std::format("partition:{}:{}", partition_index.value, partition_name)
                        : std::format("partition:{}", partition_index.value),
        "partition",
        prefix_partition_index
            ? std::format("partition {}: {}", partition_index.value, partition_name)
            : display_text(partition_name),
    };
    root.children = std::move(partition_volumes[{partition_index.value, partition_name}]);
    result.roots.push_back(std::move(root));
  }
  for (const auto &issue : catalog.issues) {
    result.issues.push_back({
        issue.code,
        "warning",
        issue.message,
        std::format("partition {}", issue.partition.value),
        issue.sfs_id ? std::format("p{}:sfs{}", issue.partition.value, issue.sfs_id->value) : "",
    });
  }
  return result;
}

ContentTree build_content_tree(const Container &container, const ObjectCatalog &catalog,
                               const RelationshipGraph &graph, bool include_default_programs) {
  std::vector<std::pair<PartitionIndex, std::string>> partitions;
  partitions.reserve(container.partitions().size());
  for (const auto &partition : container.partitions()) {
    partitions.emplace_back(partition.index, partition.name);
  }
  return build_content_tree_impl(container.source_path().string(), partitions, catalog, graph,
                                 include_default_programs, true);
}

ContentTree build_content_tree(const MediaContainer &container, const ObjectCatalog &catalog,
                               const RelationshipGraph &graph, bool include_default_programs) {
  if (const auto *sfs = std::get_if<Container>(&container.storage()))
    return build_content_tree(*sfs, catalog, graph, include_default_programs);

  auto result = build_content_tree(container.source_path().generic_string(), catalog, graph,
                                   include_default_programs);
  if (!result.roots.empty()) {
    std::set<std::string> unresolved_banks;
    std::map<std::string, std::vector<std::string>> group_banks;
    for (const auto &row : graph.relationships) {
      if ((row.type == "SBNK_LEFT_MEMBER_TO_SMPL" ||
           row.type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
          row.quality == RelationshipQuality::unknown) {
        unresolved_banks.insert(row.source_key);
      } else if (row.type == "SBAC_SLOT_TO_SBNK" && row.target_key &&
                 (row.quality == RelationshipQuality::known ||
                  row.quality == RelationshipQuality::likely)) {
        group_banks[row.source_key].push_back(*row.target_key);
      }
    }
    std::set<std::string> reachable_banks;
    for (const auto &row : graph.relationships) {
      if (!row.target_key ||
          (row.assignment_state != AssignmentState::active &&
           row.assignment_state != AssignmentState::source_load) ||
          (row.quality != RelationshipQuality::known &&
           row.quality != RelationshipQuality::likely)) {
        continue;
      }
      if (row.type == "PROG_ASSIGNMENT_TO_SBNK") {
        reachable_banks.insert(*row.target_key);
      } else if (row.type == "PROG_ASSIGNMENT_TO_SBAC") {
        if (const auto members = group_banks.find(*row.target_key); members != group_banks.end())
          reachable_banks.insert(members->second.begin(), members->second.end());
      }
    }
    std::set<std::pair<std::string, std::string>> affected_volumes;
    for (const auto &bank_key : unresolved_banks) {
      if (!reachable_banks.contains(bank_key))
        continue;
      if (const auto *bank = find_object(catalog, bank_key); bank != nullptr && bank->placement) {
        affected_volumes.emplace(display_text(bank->placement->partition_name),
                                 display_text(bank->placement->volume_name));
        result.issues.push_back({"REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING", "error",
                                 "active Program reaches a sample-bank member without one waveform",
                                 sampler_path(*bank), bank_key});
      }
    }
    for (auto &root : result.roots) {
      for (auto &volume : root.children) {
        if (affected_volumes.contains({root.display_name, volume.display_name}))
          volume.display_name += " (errors detected)";
      }
    }
    return result;
  }

  const auto name = container.kind() == MediaKind::fat12_floppy ? "FAT root"
                                                                 : "Standalone object";
  result.roots.push_back({std::format("scope:{}", name), "volume", name});
  return result;
}

ContentTree build_content_tree(std::string source_path, const ObjectCatalog &catalog,
                               const RelationshipGraph &graph, bool include_default_programs) {
  std::set<std::pair<std::uint8_t, std::string>> names;
  for (const auto &item : catalog.objects) {
    if (item.placement)
      names.emplace(item.partition.value, item.placement->partition_name);
  }
  std::vector<std::pair<PartitionIndex, std::string>> partitions;
  for (const auto &[index, name] : names) {
    partitions.emplace_back(PartitionIndex{index}, name);
  }
  auto result = build_content_tree_impl(std::move(source_path), partitions, catalog, graph,
                                        include_default_programs, false);
  std::vector<ContentNode> flattened;
  for (auto &root : result.roots) {
    if (root.display_name.empty()) {
      std::ranges::move(root.children, std::back_inserter(flattened));
    } else {
      flattened.push_back(std::move(root));
    }
  }
  result.roots = std::move(flattened);
  return result;
}

WaveformOrphanReport analyze_waveform_orphans(const Container &container,
                                              const ObjectCatalog &catalog,
                                              const RelationshipGraph &graph) {
  WaveformOrphanReport result;
  for (const auto &item : catalog.objects) {
    const auto *sample = std::get_if<CurrentSmpl>(&item.object.payload);
    if (sample == nullptr)
      continue;
    WaveformOrphanRow row;
    row.partition = item.partition;
    row.waveform_name = item.object.header.name;
    row.object_key = item.key;
    row.sfs_id = item.sfs_id;
    row.smpl_link_id = sample->link_id.value;
    if (item.placement) {
      row.partition_name = item.placement->partition_name;
      row.volume_name = item.placement->volume_name;
    }

    for (const auto *relation : graph.parents(item.key)) {
      if ((relation->type == "SBNK_LEFT_MEMBER_TO_SMPL" ||
           relation->type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
          relation->target_key && *relation->target_key == item.key &&
          relation->quality == RelationshipQuality::known) {
        if (const auto *bank = find_object(catalog, relation->source_key); bank != nullptr) {
          row.referencing_sample_banks.push_back(
              bank->placement
                  ? std::format("{}/{}", bank->placement->volume_name, bank->object.header.name)
                  : bank->object.header.name);
        }
      }
    }
    if (!row.referencing_sample_banks.empty()) {
      std::ranges::sort(row.referencing_sample_banks);
      row.referencing_sample_banks.erase(
          std::unique(row.referencing_sample_banks.begin(), row.referencing_sample_banks.end()),
          row.referencing_sample_banks.end());
      row.status = WaveformStatus::referenced;
      row.basis = "unique current SBNK member match by waveform name and SMPL link ID";
      ++result.referenced_count;
    } else {
      std::vector<std::string> blockers;
      if (!item.placement)
        blockers.emplace_back("waveform has no exact SMPL directory placement");
      if (partition_has_unknown_records(container, item.partition)) {
        blockers.emplace_back("partition contains an unresolved allocated record");
      }
      for (const auto &issue : catalog.issues) {
        if (issue.partition.value == item.partition.value)
          blockers.push_back(issue.message);
      }
      for (const auto &relation : graph.relationships) {
        if (relation.scope_key == item.scope_key &&
            (relation.type == "SBNK_LEFT_MEMBER_TO_SMPL" ||
             relation.type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
            relation.quality != RelationshipQuality::known) {
          blockers.push_back(
              std::format("sample-bank member is unresolved: {}", relation.source_key));
        }
      }
      if (blockers.empty()) {
        row.status = WaveformStatus::known_unreferenced;
        row.basis = "exact SMPL placement and complete current SBNK member resolution";
        ++result.known_unreferenced_count;
      } else {
        row.status = WaveformStatus::ambiguous_or_unresolved;
        row.basis = "orphan status withheld because partition ownership is unresolved";
        row.notes = join(blockers);
        ++result.ambiguous_or_unresolved_count;
      }
    }
    result.rows.push_back(std::move(row));
  }
  std::ranges::sort(result.rows, {}, [](const WaveformOrphanRow &row) {
    return std::tuple{row.partition.value, row.volume_name, row.waveform_name, row.sfs_id.value};
  });
  return result;
}

bool ValidationReport::valid() const noexcept {
  return std::ranges::none_of(issues, [](const ValidationIssue &issue) {
    return issue.severity == ValidationSeverity::error;
  });
}

ValidationReport validate_semantics(const Container &container, const ObjectCatalog &catalog,
                                    const RelationshipGraph &graph) {
  ValidationReport result;
  result.coverage.object_count = catalog.objects.size();
  result.coverage.relationship_count = graph.relationships.size();
  for (const auto &item : catalog.objects) {
    if (item.placement) {
      ++result.coverage.exact_placement_count;
      if (item.placement->category_name != object_type_name(item.object.header.type)) {
        result.issues.push_back({
            "VOL_OBJECT_CATEGORY_MISMATCH",
            ValidationSeverity::error,
            std::format("{} object '{}' is stored in the {} category",
                        object_type_name(item.object.header.type), item.object.header.name,
                        item.placement->category_name),
            sampler_path(item),
            item.key,
        });
      }
    } else {
      ++result.coverage.unresolved_placement_count;
    }
  }
  for (const auto &issue : catalog.issues) {
    result.issues.push_back({
        issue.code,
        ValidationSeverity::error,
        issue.message,
        std::format("partition {}", issue.partition.value),
        issue.sfs_id ? std::format("p{}:sfs{}", issue.partition.value, issue.sfs_id->value) : "",
    });
  }
  for (const auto &relation : graph.relationships) {
    switch (relation.quality) {
    case RelationshipQuality::known:
      ++result.coverage.known_relationship_count;
      break;
    case RelationshipQuality::likely:
      ++result.coverage.likely_relationship_count;
      break;
    case RelationshipQuality::tentative:
      ++result.coverage.tentative_relationship_count;
      break;
    case RelationshipQuality::unknown:
      ++result.coverage.unknown_relationship_count;
      break;
    }
    if ((relation.type == "SBNK_LEFT_MEMBER_TO_SMPL" ||
         relation.type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
        relation.quality != RelationshipQuality::known) {
      const auto *source = find_object(catalog, relation.source_key);
      result.issues.push_back({
          "REL_SBNK_MEMBER_TARGET_MISSING",
          ValidationSeverity::error,
          source == nullptr
              ? "sample-bank member does not resolve to one waveform"
              : std::format("sample bank '{}' has a member that does not resolve to one waveform",
                            source->object.header.name),
          source == nullptr ? "" : sampler_path(*source),
          relation.source_key,
      });
    }
    if (relation.type.starts_with("PROG_ASSIGNMENT_TO_") &&
        relation.assignment_state == AssignmentState::active && !relation.target_key) {
      const auto *source = find_object(catalog, relation.source_key);
      result.issues.push_back({
          "REL_ACTIVE_PROGRAM_TARGET_MISSING",
          ValidationSeverity::error,
          std::format("active Program assignment '{}' does not resolve to one local target",
                      relation.assignment_name),
          source == nullptr ? "" : sampler_path(*source),
          relation.source_key,
      });
    }
  }
  for (const auto &comparison : graph.bitmap_comparisons) {
    if (comparison.status == "match")
      continue;
    const auto *source = find_object(catalog, comparison.sbnk_key);
    result.issues.push_back({
        "REL_SBNK_PROGRAM_BITMAP_MISMATCH",
        ValidationSeverity::warning,
        "sample-bank program bitmap differs from decoded direct Program assignments",
        source == nullptr ? "" : sampler_path(*source),
        comparison.sbnk_key,
    });
  }
  for (const auto &partition : container.partitions()) {
    if (partition.allocation.invalid_extent_record_count != 0U ||
        partition.allocation.extent_total_mismatch_count != 0U ||
        !partition.allocation.stored_not_reconstructed.empty() ||
        !partition.allocation.reconstructed_not_stored.empty()) {
      result.issues.push_back({
          "SFS_ALLOCATION_MISMATCH",
          ValidationSeverity::error,
          "partition allocation metadata does not match reconstructed record allocation",
          std::format("partition {}: {}", partition.index.value, partition.name),
          {},
      });
    }
  }
  return result;
}

std::string_view waveform_status_name(WaveformStatus status) noexcept {
  switch (status) {
  case WaveformStatus::referenced:
    return "referenced";
  case WaveformStatus::known_unreferenced:
    return "known_unreferenced";
  case WaveformStatus::ambiguous_or_unresolved:
    return "ambiguous_or_unresolved";
  }
  return "ambiguous_or_unresolved";
}

} // namespace axk
