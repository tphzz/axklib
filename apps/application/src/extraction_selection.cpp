#include "axklib/application/extraction_selection.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

#include "axklib/terminology.hpp"
#include "axklib/utf8.hpp"

namespace {

bool scope_matches(const axk::ContentNode &node, std::string_view scope) {
    return (scope == "volume" && node.node_type == "volume") || (scope == "program" && node.object_type == "PROG") ||
           (scope == "sbac" && node.object_type == "SBAC") || (scope == "sbnk" && node.object_type == "SBNK");
}

std::string sfs_selector_component(const axk::ContentNode &node) {
    if (node.node_type == "partition") {
        const auto separator = node.node_id.find(':');
        const auto raw_index = separator == std::string::npos ? std::string{} : node.node_id.substr(separator + 1U);
        auto partition_name = node.display_name;
        const auto prefix = std::format("partition {}: ", raw_index);
        if (partition_name.starts_with(prefix))
            partition_name.erase(0U, prefix.size());
        std::string safe;
        bool prior_space{};
        for (const auto value : partition_name) {
            const auto byte = static_cast<unsigned char>(value);
            if (std::isspace(byte) != 0) {
                if (!safe.empty() && !prior_space)
                    safe.push_back('_');
                prior_space = true;
            } else {
                const bool retained = std::isalnum(byte) != 0 || value == '.' || value == '_' || value == '-';
                safe.push_back(retained ? value : '_');
                prior_space = false;
            }
        }
        while (!safe.empty() && (safe.front() == '.' || safe.front() == '_' || safe.front() == '-'))
            safe.erase(safe.begin());
        while (!safe.empty() && (safe.back() == '.' || safe.back() == '_' || safe.back() == '-'))
            safe.pop_back();
        return std::format("partition_{:0>2}_{}", raw_index,
                           safe.empty() ? std::format("partition_{:0>2}", raw_index) : safe);
    }
    auto result = node.display_name;
    std::ranges::replace(result, '/', '_');
    std::ranges::replace(result, '\\', '_');
    return result;
}

std::string selector_component(axk::MediaKind media_kind, const axk::ContentNode &node) {
    if (node.node_id == axk::sample_structure_category_id)
        return std::string{axk::sample_structure_selector_component};
    if (node.node_id == axk::wave_data_category_id)
        return std::string{axk::wave_data_selector_component};
    return media_kind == axk::MediaKind::sfs ? sfs_selector_component(node) : node.display_name;
}

void find_matches(axk::MediaKind media_kind, const axk::ContentNode &node, std::string_view scope,
                  std::string_view wanted, std::string parent_path, std::vector<const axk::ContentNode *> &matches) {
    const auto component = selector_component(media_kind, node);
    const auto selector = parent_path.empty() ? component : std::format("{}/{}", parent_path, component);
    if (selector == wanted && scope_matches(node, scope))
        matches.push_back(&node);
    for (const auto &child : node.children)
        find_matches(media_kind, child, scope, wanted, selector, matches);
}

axk::app::Error selection_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

} // namespace

axk::app::Result<axk::app::ExtractionSelection> axk::app::resolve_extraction_selection(MediaKind media_kind,
                                                                                       const ContentTree &tree,
                                                                                       std::string_view scope,
                                                                                       std::string_view selector_path) {
    if (scope != "volume" && scope != "program" && scope != "sbac" && scope != "sbnk") {
        return std::unexpected(
            selection_error("unsupported_selection_scope", "selection scope must be volume, program, sbac, or sbnk"));
    }
    std::vector<const ContentNode *> matches;
    for (const auto &root : tree.roots)
        find_matches(media_kind, root, scope, selector_path, {}, matches);
    if (matches.empty()) {
        return std::unexpected(selection_error(
            "selector_not_found", "selector path was not found; use the path returned by the info paths view"));
    }
    if (matches.size() != 1U) {
        return std::unexpected(
            selection_error("selector_ambiguous", "selector path resolves to more than one sampler object"));
    }
    return ExtractionSelection{matches.front()->object_key};
}

std::vector<axk::app::ExcludedExtractionRelationship>
axk::app::filter_export_plan(ExportPlan &plan, const RelationshipGraph &graph, std::string_view scope,
                             std::string_view selector_path, std::string_view selector_key) {
    if (scope == "volume") {
        std::erase_if(plan.volumes,
                      [&](const auto &volume) { return text::path_to_utf8(volume.relative_root) != selector_path; });
        plan.unresolved_wave_data.clear();
        return {};
    }
    std::set<std::string> programs;
    std::set<std::string> sample_banks;
    std::set<std::string> samples;
    if (scope == "program")
        programs.insert(std::string{selector_key});
    else if (scope == "sbac")
        sample_banks.insert(std::string{selector_key});
    else if (scope == "sbnk")
        samples.insert(std::string{selector_key});
    auto closure =
        build_exact_export_closure(graph, std::move(programs), std::move(sample_banks), std::move(samples), {});
    filter_export_plan(plan, closure);
    return closure.excluded;
}

axk::app::ExactExportClosure axk::app::build_exact_export_closure(const RelationshipGraph &graph,
                                                                  std::set<std::string> programs,
                                                                  std::set<std::string> sample_banks,
                                                                  std::set<std::string> samples,
                                                                  std::set<std::string> wave_data) {
    ExactExportClosure closure;
    closure.programs = std::move(programs);
    closure.sample_banks = std::move(sample_banks);
    closure.samples = std::move(samples);
    closure.wave_data = std::move(wave_data);
    std::set<std::string> excluded_keys;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &row : graph.relationships) {
            if (!row.target_key)
                continue;
            const auto active_program_assignment = closure.programs.contains(row.source_key) &&
                                                   row.type.starts_with("PROG_ASSIGNMENT_TO_") &&
                                                   (row.assignment_state == AssignmentState::active ||
                                                    row.assignment_state == AssignmentState::source_load);
            const auto sample_bank_member =
                closure.sample_banks.contains(row.source_key) && row.type == "SBAC_SLOT_TO_SBNK";
            const auto sample_wave_data =
                closure.samples.contains(row.source_key) &&
                (row.type == "SBNK_LEFT_MEMBER_TO_SMPL" || row.type == "SBNK_RIGHT_MEMBER_TO_SMPL");
            const auto wave_data_parent =
                closure.wave_data.contains(*row.target_key) &&
                (row.type == "SBNK_LEFT_MEMBER_TO_SMPL" || row.type == "SBNK_RIGHT_MEMBER_TO_SMPL");
            if ((active_program_assignment || sample_bank_member || sample_wave_data || wave_data_parent) &&
                row.quality != RelationshipQuality::known) {
                const auto key = row.source_key + '\0' + row.type + '\0' + *row.target_key;
                if (excluded_keys.insert(key).second)
                    closure.excluded.push_back({row.source_key, *row.target_key, row.type});
                continue;
            }
            if (active_program_assignment && row.quality == RelationshipQuality::known) {
                closure.program_targets.emplace(row.source_key, *row.target_key);
                if (row.type == "PROG_ASSIGNMENT_TO_SBAC") {
                    changed = closure.sample_banks.insert(*row.target_key).second || changed;
                } else if (row.type == "PROG_ASSIGNMENT_TO_SBNK") {
                    changed = closure.samples.insert(*row.target_key).second || changed;
                }
            }
            if (sample_bank_member && row.quality == RelationshipQuality::known) {
                closure.sample_bank_members.emplace(row.source_key, *row.target_key);
                changed = closure.samples.insert(*row.target_key).second || changed;
            }
            if (sample_wave_data && row.quality == RelationshipQuality::known) {
                closure.sample_wave_data.emplace(row.source_key, *row.target_key);
                changed = closure.wave_data.insert(*row.target_key).second || changed;
            }
            if (wave_data_parent && row.quality == RelationshipQuality::known) {
                closure.sample_wave_data.emplace(row.source_key, *row.target_key);
                changed = closure.samples.insert(row.source_key).second || changed;
            }
        }
    }
    return closure;
}

void axk::app::filter_export_plan(ExportPlan &plan, const ExactExportClosure &closure,
                                  const std::set<std::pair<std::uint8_t, std::string>> &whole_volumes) {
    for (auto &volume : plan.volumes) {
        if (whole_volumes.contains({volume.partition.value, volume.volume_name}))
            continue;
        std::erase_if(volume.samples, [&](const auto &sample) { return !closure.samples.contains(sample.object_key); });
        for (auto &sample : volume.samples) {
            const auto previous_member_count = sample.members.size();
            std::erase_if(sample.members, [&](const auto &member) {
                return member.quality != RelationshipQuality::known ||
                       !closure.sample_wave_data.contains({sample.object_key, member.waveform_key});
            });
            if (sample.members.size() != previous_member_count) {
                sample.rendered_wav_path.reset();
                sample.stereo_decision.reset();
            }
        }
        std::erase_if(volume.samples, [](const auto &sample) { return sample.members.empty(); });
        std::erase_if(volume.waveforms,
                      [&](const auto &waveform) { return !closure.wave_data.contains(waveform.object_key); });
        std::erase_if(volume.sample_banks,
                      [&](const auto &sample_bank) { return !closure.sample_banks.contains(sample_bank.object_key); });
        for (auto &sample_bank : volume.sample_banks) {
            const auto is_known_member = [&](const auto &key) {
                return closure.sample_bank_members.contains({sample_bank.object_key, key});
            };
            std::erase_if(sample_bank.member_sample_keys, std::not_fn(is_known_member));
            std::erase_if(sample_bank.relationship_sample_keys, std::not_fn(is_known_member));
        }
        std::erase_if(volume.programs,
                      [&](const auto &program) { return !closure.programs.contains(program.object_key); });
        for (auto &program : volume.programs) {
            std::erase_if(program.assignment_target_keys, [&](const auto &key) {
                return !closure.program_targets.contains({program.object_key, key});
            });
        }
    }
    std::erase_if(plan.volumes, [](const auto &volume) { return volume.samples.empty() && volume.waveforms.empty(); });
    for (auto &unresolved_scope : plan.unresolved_wave_data) {
        std::erase_if(unresolved_scope.waveforms,
                      [&](const auto &waveform) { return !closure.wave_data.contains(waveform.object_key); });
    }
    std::erase_if(plan.unresolved_wave_data, [](const auto &scope) { return scope.waveforms.empty(); });
}
