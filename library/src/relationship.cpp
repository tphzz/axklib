#include "axklib/relationship.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>

namespace axk {
namespace {

struct Match {
  const ObjectSnapshot *target{};
  std::vector<const ObjectSnapshot *> candidates;
  RelationshipQuality quality{RelationshipQuality::unknown};
  std::string basis;
  std::string notes;
};

struct ScopeIndex {
  std::map<std::pair<ObjectType, std::string>, std::vector<const ObjectSnapshot *>> typed_names;
  std::map<std::string, std::vector<const ObjectSnapshot *>> names;
  std::unordered_map<std::uint32_t, std::vector<const ObjectSnapshot *>> sample_links;
  std::unordered_map<std::string, const ObjectSnapshot *> keys;
};

ScopeIndex index_scope(const std::vector<const ObjectSnapshot *> &scope) {
  ScopeIndex index;
  for (const auto *item : scope) {
    index.typed_names[{item->object.header.type, item->object.header.name}].push_back(item);
    index.names[item->object.header.name].push_back(item);
    index.keys.emplace(item->key, item);
    if (const auto *sample = std::get_if<CurrentSmpl>(&item->object.payload))
      index.sample_links[sample->link_id.value].push_back(item);
  }
  return index;
}

ObjectType expected_type(std::uint8_t kind) {
  if (kind == 0x10U)
    return ObjectType::sbnk;
  if (kind == 0x11U)
    return ObjectType::sbac;
  return ObjectType::unknown;
}

bool same_volume(const ObjectSnapshot &left, const ObjectSnapshot &right) {
  return left.placement && right.placement &&
         left.placement->partition.value == right.placement->partition.value &&
         left.placement->volume_directory.value == right.placement->volume_directory.value;
}

bool same_container_folder(const ObjectSnapshot &left, const ObjectSnapshot &right) {
  return left.placement && right.placement && !left.placement->container_directory.empty() &&
         left.placement->container_directory == right.placement->container_directory;
}

std::vector<const ObjectSnapshot *> named(const ScopeIndex &index, ObjectType type,
                                          std::string_view name) {
  if (type == ObjectType::unknown) {
    const auto found = index.names.find(std::string{name});
    return found == index.names.end() ? std::vector<const ObjectSnapshot *>{} : found->second;
  }
  const auto found = index.typed_names.find({type, std::string{name}});
  return found == index.typed_names.end() ? std::vector<const ObjectSnapshot *>{} : found->second;
}

const ObjectSnapshot *unique_local(const ObjectSnapshot &source,
                                   const std::vector<const ObjectSnapshot *> &candidates) {
  const ObjectSnapshot *result{};
  for (const auto *candidate : candidates) {
    const bool local = source.placement && !source.placement->container_directory.empty()
                           ? same_container_folder(source, *candidate)
                           : same_volume(source, *candidate);
    if (!local)
      continue;
    if (result != nullptr)
      return nullptr;
    result = candidate;
  }
  return result;
}

std::string local_suffix(const ObjectSnapshot &source) {
  return source.placement && !source.placement->container_directory.empty() ? "same-folder"
                                                                            : "same-volume";
}

Match match_member(const ObjectSnapshot &source, const CurrentSbnkMember &member,
                   const ScopeIndex &index) {
  const auto link_entry = index.sample_links.find(member.smpl_link_id);
  const auto by_link = link_entry == index.sample_links.end()
                           ? std::vector<const ObjectSnapshot *>{}
                           : link_entry->second;
  std::vector<const ObjectSnapshot *> exact;
  for (const auto *item : by_link) {
    if (item->object.header.name == member.sample_name)
      exact.push_back(item);
  }
  if (exact.size() == 1) {
    return {exact.front(), exact, RelationshipQuality::known, "sbnk-member-link+name",
            "member name and link ID match one SMPL object"};
  }
  if (exact.size() > 1) {
    if (const auto *local = unique_local(source, exact); local != nullptr) {
      return {local, exact, RelationshipQuality::likely,
              std::format("sbnk-member-link+name+{}", local_suffix(source)),
              "duplicate member identities have one same-volume candidate"};
    }
    return {nullptr, exact, RelationshipQuality::tentative, "sbnk-member-link+name-ambiguous",
            "member name and link ID match multiple SMPL objects"};
  }
  auto by_name = named(index, ObjectType::smpl, member.sample_name);
  if (by_name.size() == 1) {
    return {by_name.front(), by_name, RelationshipQuality::likely, "sbnk-member-name-only",
            "member name uniquely matches but link ID does not confirm it"};
  }
  if (by_link.size() == 1) {
    const bool iso_cross_folder = source.scope_key.starts_with("iso:") && source.placement &&
                                  by_link.front()->placement &&
                                  !same_container_folder(source, *by_link.front());
    return {by_link.front(), by_link, RelationshipQuality::tentative,
            iso_cross_folder ? "sbnk-member-link-id-only-iso-cross-folder-name-mismatch"
                             : "sbnk-member-link-id-only-name-mismatch",
            "member link ID uniquely matches but name does not confirm it"};
  }
  if (by_name.size() > 1) {
    if (const auto *local = unique_local(source, by_name); local != nullptr) {
      return {local, by_name, RelationshipQuality::likely,
              std::format("sbnk-member-name+{}", local_suffix(source)),
              "duplicate member names have one local container candidate"};
    }
  }
  if (!by_name.empty())
    return {nullptr, by_name, RelationshipQuality::tentative, "sbnk-member-name-ambiguous",
            "member name matches multiple SMPL objects"};
  if (!by_link.empty())
    return {nullptr, by_link, RelationshipQuality::tentative, "sbnk-member-link-ambiguous",
            "member link ID matches multiple SMPL objects"};
  return {nullptr,
          {},
          RelationshipQuality::unknown,
          "sbnk-member-unmatched",
          "member does not match a SMPL object"};
}

Match match_named_target(const ObjectSnapshot &source, std::string_view name, ObjectType type,
                         const ScopeIndex &index, std::string_view unique_basis,
                         std::string_view ambiguous_basis, bool use_same_volume) {
  auto candidates = named(index, type, name);
  if (candidates.size() == 1) {
    if (unique_basis.starts_with("assignment-kind-0x")) {
      const auto category = type == ObjectType::sbac   ? "SBAC"
                            : type == ObjectType::sbnk ? "SBNK"
                                                       : "object";
      const auto selector = unique_basis.substr(std::string_view{"assignment-kind-"}.size(), 4U);
      return {candidates.front(), candidates, RelationshipQuality::known, std::string{unique_basis},
              std::format("Input consistency: assignment name matches a same-scope object, and "
                          "assignment kind byte {} selects {} in tested current-object corpora. "
                          "Keep the selector below write-side quality until formula or validated "
                          "saves support it.",
                          selector, category)};
    }
    return {candidates.front(), candidates, RelationshipQuality::known, std::string{unique_basis},
            "name and target category match one object"};
  }
  if (candidates.size() > 1 && use_same_volume) {
    if (const auto *local = unique_local(source, candidates); local != nullptr) {
      return {local, candidates, RelationshipQuality::likely,
              std::format("{}+{}", unique_basis, local_suffix(source)),
              "duplicate names have one same-volume candidate"};
    }
  }
  return {nullptr, candidates,
          candidates.empty() ? RelationshipQuality::unknown : RelationshipQuality::tentative,
          candidates.empty() ? unique_basis == "active-sbac-slot-name"
                                   ? "active-sbac-slot-unmatched"
                                   : std::format("{}-unmatched", unique_basis)
                             : std::string{ambiguous_basis},
          candidates.empty() ? "no same-scope target has this name"
                             : "multiple same-scope targets have this name"};
}

std::vector<std::string> keys(const Match &match) {
  std::vector<std::string> result;
  result.reserve(match.candidates.size());
  for (const auto *item : match.candidates)
    result.push_back(item->key);
  return result;
}

Relationship edge(const ObjectSnapshot &source, std::string type, const Match &match,
                  std::optional<std::size_t> assignment_index = std::nullopt,
                  std::string assignment_name = {},
                  AssignmentState assignment_state = AssignmentState::unknown,
                  std::string receive_channel_display = {}) {
  const auto target = match.target ? std::optional<std::string>{match.target->key} : std::nullopt;
  const auto discriminator = target.value_or(match.candidates.empty() ? "missing" : "candidates");
  return {
      std::format("{}|{}|{}|{}", source.key, type, discriminator, match.basis),
      source.key,
      target,
      keys(match),
      std::move(type),
      match.quality,
      match.basis,
      match.notes,
      source.scope_key,
      assignment_index,
      std::move(assignment_name),
      assignment_state,
      std::move(receive_channel_display),
  };
}

AssignmentState assignment_state(const ProgAssignment &row) {
  const auto gate = std::to_integer<std::uint8_t>(row.raw_row[0x28]);
  if (gate == 0xffU)
    return AssignmentState::active;
  if (gate == 0U)
    return AssignmentState::visible_off;
  return AssignmentState::unknown;
}

std::string receive_channel(const ProgAssignment &row) {
  const auto gate = std::to_integer<std::uint8_t>(row.raw_row[0x28]);
  if (gate == 0U)
    return "off";
  if (gate != 0xffU)
    return "unknown";
  if (row.flags == 0xffU)
    return "=SMP";
  if (row.flags <= 15U)
    return std::format("{:02}", row.flags + 1U);
  if (row.flags == 16U)
    return "BasicRch";
  if (row.flags <= 32U)
    return std::format("B{:02}", row.flags - 16U);
  return "unknown";
}

std::optional<std::uint8_t> program_number(const ObjectSnapshot &item) {
  if (item.object.header.type != ObjectType::prog)
    return std::nullopt;
  unsigned int value{};
  const auto &name = item.object.header.name;
  const auto result = std::from_chars(name.data(), name.data() + name.size(), value);
  if (result.ec != std::errc{} || result.ptr != name.data() + name.size() || value < 1U ||
      value > 128U) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> sorted_unique(std::vector<std::uint8_t> values) {
  std::ranges::sort(values);
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

} // namespace

std::vector<const Relationship *> RelationshipGraph::children(std::string_view key) const {
  std::vector<const Relationship *> result;
  for (const auto &row : relationships) {
    if (row.source_key == key)
      result.push_back(&row);
  }
  return result;
}

std::vector<const Relationship *> RelationshipGraph::parents(std::string_view key) const {
  std::vector<const Relationship *> result;
  for (const auto &row : relationships) {
    if ((row.target_key && *row.target_key == key) ||
        std::ranges::find(row.candidate_keys, key) != row.candidate_keys.end()) {
      result.push_back(&row);
    }
  }
  return result;
}

RelationshipGraph build_relationship_graph(const ObjectCatalog &catalog) {
  RelationshipGraph result;
  std::map<std::string, std::vector<const ObjectSnapshot *>> scopes;
  for (const auto &item : catalog.objects)
    scopes[item.scope_key].push_back(&item);

  for (const auto &[scope_key, scope] : scopes) {
    static_cast<void>(scope_key);
    const auto scope_index = index_scope(scope);
    for (const auto *item : scope) {
      if (const auto *bank = std::get_if<CurrentSbnk>(&item->object.payload)) {
        if (!bank->left.sample_name.empty()) {
          result.relationships.push_back(edge(*item, "SBNK_LEFT_MEMBER_TO_SMPL",
                                              match_member(*item, bank->left, scope_index)));
        }
        if (bank->right && !bank->right->sample_name.empty()) {
          result.relationships.push_back(edge(*item, "SBNK_RIGHT_MEMBER_TO_SMPL",
                                              match_member(*item, *bank->right, scope_index)));
        }
      } else if (const auto *group = std::get_if<CurrentSbac>(&item->object.payload)) {
        for (const auto &slot : group->slots) {
          if (slot.name.empty())
            continue;
          result.relationships.push_back(
              edge(*item, "SBAC_SLOT_TO_SBNK",
                   match_named_target(*item, slot.name, ObjectType::sbnk, scope_index,
                                      "active-sbac-slot-name", "active-sbac-slot-name-ambiguous",
                                      true)));
        }
      } else if (const auto *program = std::get_if<CurrentProg>(&item->object.payload)) {
        const auto first_program_edge = result.relationships.size();
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
          const auto &row = program->assignments[index];
          if (row.name.empty())
            continue;
          const auto type = expected_type(row.kind);
          if (type == ObjectType::unknown)
            continue;
          auto match = match_named_target(
              *item, row.name, type, scope_index,
              type == ObjectType::unknown ? "assignment-name-unique"
                                          : std::format("assignment-kind-0x{:02x}+name", row.kind),
              type == ObjectType::unknown
                  ? "assignment-name-ambiguous"
                  : std::format("assignment-kind-0x{:02x}+name-ambiguous", row.kind),
              assignment_state(row) == AssignmentState::active ||
                  (item->placement && !item->placement->container_directory.empty()));
          if (match.target != nullptr && match.basis.ends_with("+same-folder") &&
              type != ObjectType::unknown) {
            match.quality = RelationshipQuality::known;
          }
          if (match.quality == RelationshipQuality::unknown && type == ObjectType::sbnk &&
              item->placement && !item->placement->container_directory.empty()) {
            auto source_load_candidates = named(scope_index, ObjectType::unknown, row.name);
            const auto *local = unique_local(*item, source_load_candidates);
            if (local != nullptr) {
              match = {local, std::move(source_load_candidates), RelationshipQuality::likely,
                       "assignment-name-unique",
                       "source-load assignment uniquely matches an object in the same folder"};
            } else if (source_load_candidates.size() > 1U) {
              match = {nullptr, std::move(source_load_candidates), RelationshipQuality::tentative,
                       "assignment-name-ambiguous",
                       "multiple same-scope non-SBNK objects match the assignment name"};
            }
          }
          if (match.quality == RelationshipQuality::unknown && row.raw_handle == 0U)
            continue;
          const auto rel_type = type == ObjectType::sbac   ? "PROG_ASSIGNMENT_TO_SBAC"
                                : type == ObjectType::sbnk ? "PROG_ASSIGNMENT_TO_SBNK"
                                                           : "PROG_ASSIGNMENT_TO_OBJECT";
          auto state = assignment_state(row);
          auto channel = receive_channel(row);
          if (match.target != nullptr && item->placement &&
              !item->placement->container_directory.empty() && state != AssignmentState::active) {
            state = AssignmentState::source_load;
            channel = "=SMP";
          }
          result.relationships.push_back(
              edge(*item, rel_type, match, index, row.name, state, std::move(channel)));
        }
        const auto last_program_edge = result.relationships.size();
        for (std::size_t edge_index = first_program_edge; edge_index < last_program_edge;
             ++edge_index) {
          auto &relationship = result.relationships[edge_index];
          if (relationship.assignment_state != AssignmentState::visible_off ||
              relationship.target_key || relationship.quality != RelationshipQuality::tentative) {
            continue;
          }
          if (relationship.basis == "assignment-kind-0x11+name-ambiguous") {
            relationship.basis = "assignment-visible-off-name-ambiguous-sbac";
          } else if (relationship.basis == "assignment-kind-0x10+name-ambiguous") {
            relationship.basis = "assignment-visible-off-name-ambiguous-sbnk";
          } else if (relationship.basis == "assignment-name-ambiguous") {
            bool only_waveforms = !relationship.candidate_keys.empty();
            for (const auto &key : relationship.candidate_keys) {
              const auto candidate = scope_index.keys.find(key);
              only_waveforms = only_waveforms && candidate != scope_index.keys.end() &&
                               candidate->second->object.header.type == ObjectType::smpl;
            }
            relationship.basis = only_waveforms
                                     ? "assignment-visible-off-name-ambiguous-smpl-candidates"
                                     : "assignment-visible-off-name-ambiguous-non-target-category";
          } else {
            continue;
          }
          relationship.key = std::format("{}|{}|ambiguous|{}", relationship.source_key,
                                         relationship.type, relationship.basis);
        }
        for (const auto rel_type : {std::string_view{"PROG_ASSIGNMENT_TO_SBNK"},
                                    std::string_view{"PROG_ASSIGNMENT_TO_SBAC"}}) {
          std::set<std::string> resolved_targets;
          for (std::size_t edge_index = first_program_edge; edge_index < last_program_edge;
               ++edge_index) {
            const auto &relationship = result.relationships[edge_index];
            if (relationship.type == rel_type && relationship.target_key &&
                (relationship.quality == RelationshipQuality::known ||
                 relationship.quality == RelationshipQuality::likely)) {
              resolved_targets.insert(*relationship.target_key);
            }
          }
          if (resolved_targets.size() != 1U)
            continue;
          const auto target_key = *resolved_targets.begin();
          for (std::size_t edge_index = first_program_edge; edge_index < last_program_edge;
               ++edge_index) {
            auto &relationship = result.relationships[edge_index];
            if (relationship.type != rel_type || relationship.target_key ||
                !relationship.candidate_keys.empty() ||
                relationship.quality != RelationshipQuality::unknown)
              continue;
            relationship.target_key = target_key;
            relationship.candidate_keys = {target_key};
            relationship.quality = RelationshipQuality::likely;
            const auto kind = rel_type.ends_with("SBAC") ? 0x11U : 0x10U;
            relationship.basis =
                std::format("assignment-kind-0x{:02x}+program-local-target-context", kind);
            relationship.notes = "unmatched assignment shares a Program with one resolved target";
            relationship.key = std::format("{}|{}|{}|{}", relationship.source_key,
                                           relationship.type, target_key, relationship.basis);
          }
        }
        for (std::size_t edge_index = first_program_edge; edge_index < last_program_edge;
             ++edge_index) {
          auto &relationship = result.relationships[edge_index];
          if (relationship.quality != RelationshipQuality::unknown ||
              relationship.assignment_state != AssignmentState::visible_off)
            continue;
          if (relationship.type == "PROG_ASSIGNMENT_TO_SBAC") {
            relationship.basis = item->scope_key.starts_with("iso:")
                                     ? "assignment-visible-off-iso-missing-local-sbac"
                                     : "assignment-visible-off-missing-local-sbac";
          } else if (relationship.type == "PROG_ASSIGNMENT_TO_SBNK") {
            relationship.basis = "assignment-visible-off-missing-local-sbnk";
          }
          relationship.key = std::format("{}|{}|missing|{}", relationship.source_key,
                                         relationship.type, relationship.basis);
        }
        std::set<std::tuple<std::string, std::string, std::string>> active_targets;
        for (std::size_t edge_index = first_program_edge; edge_index < last_program_edge;
             ++edge_index) {
          auto &relationship = result.relationships[edge_index];
          if (!relationship.target_key ||
              (relationship.assignment_state != AssignmentState::source_load &&
               relationship.assignment_state != AssignmentState::visible_off)) {
            continue;
          }
          const auto identity =
              std::tuple{relationship.type, *relationship.target_key, relationship.assignment_name};
          if (!active_targets.insert(identity).second)
            relationship.assignment_state = AssignmentState::duplicate_not_active;
        }
      }
    }

    std::unordered_map<std::string, std::vector<std::string>> group_banks;
    for (const auto &row : result.relationships) {
      if (row.scope_key == scope_key && row.type == "SBAC_SLOT_TO_SBNK" && row.target_key &&
          row.quality == RelationshipQuality::known) {
        group_banks[row.source_key].push_back(*row.target_key);
      }
    }
    std::unordered_map<std::string, std::vector<std::uint8_t>> direct_programs;
    std::unordered_map<std::string, std::vector<std::uint8_t>> indirect_programs;
    std::unordered_map<std::string, std::vector<std::uint8_t>> ambiguous_programs;
    std::unordered_map<std::string, std::map<std::uint8_t, bool>> direct_nondefault_flags;
    for (const auto &row : result.relationships) {
      if (row.scope_key != scope_key)
        continue;
      const auto source = scope_index.keys.find(row.source_key);
      if (source == scope_index.keys.end())
        continue;
      const auto number = program_number(*source->second);
      if (!number)
        continue;
      if (row.type == "PROG_ASSIGNMENT_TO_SBNK" && row.target_key) {
        direct_programs[*row.target_key].push_back(*number);
        bool nondefault = false;
        if (row.assignment_index) {
          if (const auto *program = std::get_if<CurrentProg>(&source->second->object.payload);
              program != nullptr && *row.assignment_index < program->assignments.size()) {
            nondefault = program->assignments[*row.assignment_index].flags != 0xffU;
          }
        }
        auto &[program_number, all_nondefault] =
            *direct_nondefault_flags[*row.target_key].try_emplace(*number, true).first;
        static_cast<void>(program_number);
        all_nondefault = all_nondefault && nondefault;
      } else if (row.type == "PROG_ASSIGNMENT_TO_SBNK" &&
                 row.quality == RelationshipQuality::tentative) {
        for (const auto &candidate : row.candidate_keys)
          ambiguous_programs[candidate].push_back(*number);
      } else if (row.type == "PROG_ASSIGNMENT_TO_SBAC" && row.target_key) {
        const auto members = group_banks.find(*row.target_key);
        if (members == group_banks.end())
          continue;
        for (const auto &bank_key : members->second)
          indirect_programs[bank_key].push_back(*number);
      }
    }

    for (const auto *item : scope) {
      const auto *bank = std::get_if<CurrentSbnk>(&item->object.payload);
      if (bank == nullptr)
        continue;
      BitmapComparison comparison;
      comparison.sbnk_key = item->key;
      comparison.bitmap_programs = bank->linked_program_numbers;
      comparison.direct_assignment_programs = direct_programs[item->key];
      comparison.indirect_assignment_programs = indirect_programs[item->key];
      comparison.direct_assignment_programs = sorted_unique(comparison.direct_assignment_programs);
      comparison.indirect_assignment_programs =
          sorted_unique(comparison.indirect_assignment_programs);
      const auto ambiguous = sorted_unique(ambiguous_programs[item->key]);
      std::ranges::set_difference(comparison.bitmap_programs, comparison.direct_assignment_programs,
                                  std::back_inserter(comparison.bitmap_without_direct));
      std::ranges::set_difference(comparison.direct_assignment_programs, comparison.bitmap_programs,
                                  std::back_inserter(comparison.direct_without_bitmap));
      if (comparison.bitmap_without_direct.empty() && comparison.direct_without_bitmap.empty()) {
        comparison.status = "match";
        comparison.mismatch_class = "match";
      } else {
        comparison.status = "mismatch";
        if (comparison.direct_without_bitmap.empty()) {
          const auto covered = std::ranges::includes(ambiguous, comparison.bitmap_without_direct);
          comparison.mismatch_class = covered ? "bitmap_disambiguates_ambiguous_direct_assignment"
                                              : "bitmap_without_decoded_direct_assignment";
        } else {
          std::set<std::string> classes;
          const auto indirect =
              std::set<std::uint8_t>(comparison.indirect_assignment_programs.begin(),
                                     comparison.indirect_assignment_programs.end());
          for (const auto number : comparison.direct_without_bitmap) {
            if (indirect.contains(number)) {
              classes.insert("direct_assignment_also_reached_through_sbac");
            } else if (direct_nondefault_flags[item->key][number]) {
              classes.insert("nondefault_flag_direct_assignment_without_bitmap");
            } else {
              classes.insert("known_direct_assignment_missing_bitmap");
            }
          }
          if (!comparison.bitmap_without_direct.empty())
            classes.insert("bitmap_without_decoded_direct_assignment");
          if (classes.size() == 1U) {
            comparison.mismatch_class = *classes.begin();
          } else {
            comparison.mismatch_class = "mixed:";
            for (const auto &value : classes) {
              if (!comparison.mismatch_class.ends_with(':'))
                comparison.mismatch_class += '+';
              comparison.mismatch_class += value;
            }
          }
        }
      }
      if (!comparison.bitmap_programs.empty()) {
        std::string targets;
        for (const auto number : comparison.bitmap_programs) {
          if (!targets.empty())
            targets += '|';
          targets += std::format("{:03}", number);
        }
        const auto quality = comparison.status == "match" ? RelationshipQuality::known
                                                          : RelationshipQuality::tentative;
        auto mismatch_basis = comparison.mismatch_class;
        std::ranges::replace(mismatch_basis, '_', '-');
        std::ranges::replace(mismatch_basis, ':', '-');
        std::ranges::replace(mismatch_basis, '+', '-');
        const auto basis =
            comparison.status == "match"
                ? std::string{"program-link-bitmap"}
                : std::format("sbnk-program-link-bitmap-{}-diagnostic", mismatch_basis);
        result.relationships.push_back({
            std::format("{}|SBNK_PROGRAM_BITMAP_TO_PROG|{}|{}", item->key, targets, basis),
            item->key,
            targets,
            {},
            "SBNK_PROGRAM_BITMAP_TO_PROG",
            quality,
            basis,
            "program-link bitmap cross-check",
            item->scope_key,
            std::nullopt,
            {},
            AssignmentState::unknown,
            {},
        });
      }
      result.bitmap_comparisons.push_back(std::move(comparison));
    }
  }

  std::ranges::sort(result.relationships, {}, [](const Relationship &row) {
    return std::tuple{row.scope_key, row.source_key, row.type, row.key};
  });
  return result;
}

std::string_view relationship_quality_name(RelationshipQuality quality) noexcept {
  switch (quality) {
  case RelationshipQuality::known:
    return "Known";
  case RelationshipQuality::likely:
    return "Likely";
  case RelationshipQuality::tentative:
    return "Tentative";
  case RelationshipQuality::unknown:
    return "Unknown";
  }
  return "Unknown";
}

std::string_view assignment_state_name(AssignmentState state) noexcept {
  switch (state) {
  case AssignmentState::active:
    return "confirmed-active";
  case AssignmentState::source_load:
    return "source-load-assignment";
  case AssignmentState::visible_off:
    return "confirmed-visible-off";
  case AssignmentState::duplicate_not_active:
    return "confirmed-duplicate-not-active";
  case AssignmentState::unknown:
    return "unknown";
  }
  return "unknown";
}

} // namespace axk
