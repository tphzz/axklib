#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

#include "content_id.hpp"
#include "handlers.hpp"
#include "requests.hpp"
#include "schema/operations_v1.hpp"
#include "support.hpp"

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/error.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"

namespace axk::cli::commands {

std::string safe_display_path_name(std::string_view value, std::string_view fallback) {
  auto text = std::string{value};
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  text = first == std::string::npos ? std::string{fallback} : text.substr(first, last - first + 1U);
  std::size_t stars{};
  while (!text.empty() && text.back() == '*') {
    ++stars;
    text.pop_back();
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    text.pop_back();
  std::string result;
  bool prior_space{};
  bool prior_underscore{};
  for (const auto character : text) {
    if (character == '<') {
      result += "_lt_";
      prior_underscore = true;
      prior_space = false;
    } else if (character == '>') {
      result += "_gt_";
      prior_underscore = true;
      prior_space = false;
    } else if (std::string_view{"\\/:*?\"|"}.contains(character) ||
               static_cast<unsigned char>(character) < 0x20U) {
      if (!prior_underscore)
        result.push_back('_');
      prior_underscore = true;
      prior_space = false;
    } else if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      if (!result.empty() && !prior_space)
        result.push_back(' ');
      prior_space = true;
      prior_underscore = false;
    } else {
      result.push_back(character);
      prior_space = false;
      prior_underscore = character == '_';
    }
  }
  while (!result.empty() && (result.back() == ' ' || result.back() == '.' || result.back() == '_'))
    result.pop_back();
  while (!result.empty() &&
         (result.front() == ' ' || result.front() == '.' || result.front() == '_'))
    result.erase(result.begin());
  if (result.empty())
    result = fallback;
  if (stars != 0U)
    result += std::format(" ({})", stars + 1U);
  return result;
}

axk::Result<void> retarget_export_plan(axk::ExportPlan &plan,
                                       const std::filesystem::path &selection_root) {
  axk::cli::detail::PooledPathAllocator pooled_paths;
  for (auto &volume : plan.volumes) {
    volume.relative_root = selection_root;
    std::map<std::string, std::filesystem::path> waveform_paths;
    for (auto &waveform : volume.waveforms) {
      auto bytes = axk::wav_bytes(waveform.waveform);
      if (!bytes)
        return std::unexpected{bytes.error()};
      auto pooled =
          pooled_paths.allocate(selection_root, "physical",
                                safe_display_path_name(waveform.display_name, "sample"), *bytes);
      if (!pooled)
        return std::unexpected{pooled.error()};
      waveform.relative_wav_path = std::move(*pooled);
      waveform_paths.emplace(waveform.object_key, waveform.relative_wav_path);
    }
    for (auto &bank : volume.sample_banks) {
      for (auto &member : bank.members) {
        if (const auto found = waveform_paths.find(member.waveform_key);
            found != waveform_paths.end())
          member.relative_wav_path = found->second;
      }
      if (bank.rendered_wav_path && bank.members.size() == 2U) {
        const auto left = std::ranges::find(volume.waveforms, bank.members[0].waveform_key,
                                            &axk::PhysicalWaveformExport::object_key);
        const auto right = std::ranges::find(volume.waveforms, bank.members[1].waveform_key,
                                             &axk::PhysicalWaveformExport::object_key);
        if (left != volume.waveforms.end() && right != volume.waveforms.end()) {
          auto stereo = axk::render_stereo(left->waveform, right->waveform);
          if (!stereo)
            return std::unexpected{stereo.error()};
          auto bytes = axk::wav_bytes(*stereo);
          if (!bytes)
            return std::unexpected{bytes.error()};
          auto pooled =
              pooled_paths.allocate(selection_root, "rendered",
                                    safe_display_path_name(bank.display_name, "sample"), *bytes);
          if (!pooled)
            return std::unexpected{pooled.error()};
          bank.rendered_wav_path = std::move(*pooled);
        }
      }
    }
  }
  return {};
}

bool selector_scope_matches(const axk::ContentNode &node, std::string_view scope) {
  return (scope == "volume" && node.node_type == "volume") ||
         (scope == "program" && node.object_type == "PROG") ||
         (scope == "sbac" && node.object_type == "SBAC") ||
         (scope == "sbnk" && node.object_type == "SBNK");
}

void find_selector_nodes(const CliLoaded &loaded, const axk::ContentNode &node,
                         std::string_view scope, std::string_view wanted, std::string parent_path,
                         std::vector<const axk::ContentNode *> &matches) {
  const auto component =
      loaded.media.kind() == axk::MediaKind::sfs ? sfs_selector_component(node) : node.display_name;
  const auto selector =
      parent_path.empty() ? component : std::format("{}/{}", parent_path, component);
  if (selector == wanted && selector_scope_matches(node, scope))
    matches.push_back(&node);
  for (const auto &child : node.children)
    find_selector_nodes(loaded, child, scope, wanted, selector, matches);
}

std::filesystem::path selection_root(std::string_view scope, std::string_view selector) {
  if (scope == "file")
    return "file";
  std::filesystem::path result{scope};
  std::string value{selector};
  std::size_t start{};
  while (start <= value.size()) {
    const auto end = value.find('/', start);
    const auto component =
        value.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!component.empty())
      result /= safe_display_path_name(component, scope);
    if (end == std::string::npos)
      break;
    start = end + 1U;
  }
  return result;
}

void filter_export_plan(axk::ExportPlan &plan, const CliLoaded &source, std::string_view scope,
                        std::string_view selector_path, std::string_view selector_key) {
  if (scope == "volume") {
    std::erase_if(plan.volumes, [&](const auto &volume) {
      return axk::text::path_to_utf8(volume.relative_root) != selector_path;
    });
    return;
  }
  std::set<std::string> programs;
  std::set<std::string> groups;
  std::set<std::string> banks;
  if (scope == "program")
    programs.insert(std::string{selector_key});
  else if (scope == "sbac")
    groups.insert(std::string{selector_key});
  else if (scope == "sbnk")
    banks.insert(std::string{selector_key});
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &row : source.graph.relationships) {
      if (!row.target_key)
        continue;
      if (programs.contains(row.source_key) && row.type.starts_with("PROG_ASSIGNMENT_TO_") &&
          (row.assignment_state == axk::AssignmentState::active ||
           row.assignment_state == axk::AssignmentState::source_load)) {
        if (row.type == "PROG_ASSIGNMENT_TO_SBAC")
          changed = groups.insert(*row.target_key).second || changed;
        else if (row.type == "PROG_ASSIGNMENT_TO_SBNK")
          changed = banks.insert(*row.target_key).second || changed;
      }
      if (groups.contains(row.source_key) && row.type == "SBAC_SLOT_TO_SBNK" &&
          (row.quality == axk::RelationshipQuality::known ||
           row.quality == axk::RelationshipQuality::likely))
        changed = banks.insert(*row.target_key).second || changed;
    }
  }
  for (auto &volume : plan.volumes) {
    std::erase_if(volume.sample_banks,
                  [&](const auto &bank) { return !banks.contains(bank.object_key); });
    std::set<std::string> waveforms;
    for (const auto &bank : volume.sample_banks) {
      for (const auto &member : bank.members)
        waveforms.insert(member.waveform_key);
    }
    std::erase_if(volume.waveforms,
                  [&](const auto &waveform) { return !waveforms.contains(waveform.object_key); });
    std::erase_if(volume.sample_bank_groups,
                  [&](const auto &group) { return !groups.contains(group.object_key); });
    for (auto &group : volume.sample_bank_groups) {
      std::erase_if(group.member_bank_keys, [&](const auto &key) { return !banks.contains(key); });
    }
    std::erase_if(volume.programs,
                  [&](const auto &program) { return !programs.contains(program.object_key); });
  }
  std::erase_if(plan.volumes, [](const auto &volume) {
    return volume.waveforms.empty() && volume.sample_banks.empty();
  });
}

int run_extract_request(const axk::cli::ExtractRequest &request) {
  if (request.scope != "file" && request.selector_paths.empty()) {
    std::cerr << "extract " << request.scope
              << " requires at least one --path from `axklib info --format paths`\n";
    return 4;
  }
  std::error_code error;
  if (std::filesystem::exists(request.output_directory, error) && !request.overwrite &&
      std::filesystem::directory_iterator{request.output_directory, error} !=
          std::filesystem::directory_iterator{}) {
    std::cerr << "error: output directory already exists and is not empty: "
              << axk::text::path_to_utf8(request.output_directory) << '\n';
    return 1;
  }
  std::filesystem::create_directories(request.output_directory, error);
  if (error) {
    std::cerr << "error: could not create export output directory\n";
    return 1;
  }
  const auto loaded = load_cli_paths(request.paths);
  axk::ExportPlan combined;
  const auto selectors =
      request.scope == "file" ? std::vector<std::string>{""} : request.selector_paths;
  for (const auto &selector : selectors) {
    bool found = request.scope == "file";
    for (const auto &source : loaded.loaded) {
      std::vector<const axk::ContentNode *> matches;
      const auto tree = cli_content_tree(source, false);
      if (request.scope != "file") {
        for (const auto &root : tree.roots)
          find_selector_nodes(source, root, request.scope, selector, {}, matches);
        if (matches.empty())
          continue;
        found = true;
      }
      auto plan = axk::build_export_plan(source.media, source.catalog, source.graph);
      if (!plan)
        return report_failure(plan.error());
      if (request.scope != "file")
        filter_export_plan(*plan, source, request.scope, selector, matches.front()->object_key);
      if (auto retargeted = retarget_export_plan(*plan, selection_root(request.scope, selector));
          !retargeted)
        return report_failure(retargeted.error());
      std::ranges::move(plan->volumes, std::back_inserter(combined.volumes));
    }
    if (!found) {
      std::cerr << "selector path not found for " << request.scope << ": " << selector
                << ". Run `axklib info --format paths` and copy the path column.\n";
      return 4;
    }
  }
  auto audio = axk::write_export_audio(combined, request.output_directory, request.overwrite);
  if (!audio)
    return report_failure(audio.error());
  std::size_t sfz_count{};
  std::size_t written_files = audio->written_files.size();
  if (request.sfz) {
    auto sfz = axk::write_sfz(combined, request.output_directory, request.overwrite);
    if (!sfz)
      return report_failure(sfz.error());
    sfz_count = sfz->written_files.size();
    written_files += sfz_count;
  }
  const auto waveform_count = std::accumulate(
      combined.volumes.begin(), combined.volumes.end(), std::size_t{},
      [](std::size_t count, const auto &volume) { return count + volume.waveforms.size(); });
  std::cout << "waveforms=" << waveform_count << " written_files=" << written_files
            << " selection_graphs=1 sfz_files=" << sfz_count
            << " decode_errors=0 load_errors=" << loaded.errors.size() << '\n';
  return loaded.errors.empty() ? 0 : 1;
}

axk::ReportRow coverage_summary(const CliLoadResult &loaded,
                                std::span<const axk::ReportRow> relationships) {
  std::map<std::string, std::uint64_t> qualities;
  std::map<std::string, std::uint64_t> types;
  std::uint64_t sbac{};
  std::uint64_t program{};
  std::uint64_t bitmaps{};
  for (const auto &source : loaded.loaded) {
    for (const auto &row : source.graph.relationships) {
      ++qualities[std::string{axk::relationship_quality_name(row.quality)}];
      ++types[row.type];
      if (row.type == "SBAC_SLOT_TO_SBNK")
        ++sbac;
      if (row.type.starts_with("PROG_ASSIGNMENT_TO_"))
        ++program;
    }
    bitmaps += source.graph.bitmap_comparisons.size();
  }
  const auto joined = [](const auto &counts) {
    std::string result;
    for (const auto &[name, count] : counts) {
      if (count == 0U)
        continue;
      if (!result.empty())
        result += ';';
      result += std::format("{}:{}", name, count);
    }
    return result;
  };
  return {{"relationship_count", static_cast<std::uint64_t>(relationships.size())},
          {"known_relationship_count", qualities["Known"]},
          {"likely_relationship_count", qualities["Likely"]},
          {"tentative_relationship_count", qualities["Tentative"]},
          {"unknown_relationship_count", qualities["Unknown"]},
          {"ambiguous_relationship_count", qualities["Tentative"]},
          {"sbac_sbnk_row_count", sbac},
          {"prog_assignment_row_count", program},
          {"prog_ignored_row_count", std::uint64_t{0}},
          {"sbnk_bitmap_row_count", bitmaps},
          {"relationship_type_counts", joined(types)},
          {"quality_counts", joined(qualities)},
          {"load_error_count", static_cast<std::uint64_t>(loaded.errors.size())}};
}

} // namespace axk::cli::commands
