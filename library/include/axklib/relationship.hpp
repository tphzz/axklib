#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "axklib/catalog.hpp"
#include "axklib/export.hpp"

namespace axk {

enum class RelationshipQuality : std::uint8_t { known, likely, tentative, unknown };
enum class AssignmentState : std::uint8_t {
  active,
  source_load,
  visible_off,
  duplicate_not_active,
  unknown
};

struct Relationship {
  std::string key;
  std::string source_key;
  std::optional<std::string> target_key;
  std::vector<std::string> candidate_keys;
  std::string type;
  RelationshipQuality quality{RelationshipQuality::unknown};
  std::string basis;
  std::string notes;
  std::string scope_key;
  std::optional<std::size_t> assignment_index;
  std::string assignment_name;
  AssignmentState assignment_state{AssignmentState::unknown};
  std::string receive_channel_display;
};

struct BitmapComparison {
  std::string sbnk_key;
  std::vector<std::uint8_t> bitmap_programs;
  std::vector<std::uint8_t> direct_assignment_programs;
  std::vector<std::uint8_t> indirect_assignment_programs;
  std::vector<std::uint8_t> bitmap_without_direct;
  std::vector<std::uint8_t> direct_without_bitmap;
  std::string status;
  std::string mismatch_class;
};

struct RelationshipGraph {
  std::vector<Relationship> relationships;
  std::vector<BitmapComparison> bitmap_comparisons;

  [[nodiscard]] std::vector<const Relationship *> children(std::string_view key) const;
  [[nodiscard]] std::vector<const Relationship *> parents(std::string_view key) const;
};

AXK_API RelationshipGraph build_relationship_graph(const ObjectCatalog &catalog);
AXK_API std::string_view relationship_quality_name(RelationshipQuality quality) noexcept;
AXK_API std::string_view assignment_state_name(AssignmentState state) noexcept;

} // namespace axk
