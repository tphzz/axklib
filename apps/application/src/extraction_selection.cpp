#include "axklib/application/extraction_selection.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <vector>

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

void axk::app::filter_export_plan(ExportPlan &plan, const RelationshipGraph &graph, std::string_view scope,
                                  std::string_view selector_path, std::string_view selector_key) {
    if (scope == "volume") {
        std::erase_if(plan.volumes,
                      [&](const auto &volume) { return text::path_to_utf8(volume.relative_root) != selector_path; });
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
        for (const auto &row : graph.relationships) {
            if (!row.target_key)
                continue;
            if (programs.contains(row.source_key) && row.type.starts_with("PROG_ASSIGNMENT_TO_") &&
                (row.assignment_state == AssignmentState::active ||
                 row.assignment_state == AssignmentState::source_load)) {
                if (row.type == "PROG_ASSIGNMENT_TO_SBAC")
                    changed = groups.insert(*row.target_key).second || changed;
                else if (row.type == "PROG_ASSIGNMENT_TO_SBNK")
                    changed = banks.insert(*row.target_key).second || changed;
            }
            if (groups.contains(row.source_key) && row.type == "SBAC_SLOT_TO_SBNK" &&
                (row.quality == RelationshipQuality::known || row.quality == RelationshipQuality::likely)) {
                changed = banks.insert(*row.target_key).second || changed;
            }
        }
    }
    for (auto &volume : plan.volumes) {
        std::erase_if(volume.sample_banks, [&](const auto &bank) { return !banks.contains(bank.object_key); });
        std::set<std::string> waveforms;
        for (const auto &bank : volume.sample_banks) {
            for (const auto &member : bank.members)
                waveforms.insert(member.waveform_key);
        }
        std::erase_if(volume.waveforms, [&](const auto &waveform) { return !waveforms.contains(waveform.object_key); });
        std::erase_if(volume.sample_bank_groups, [&](const auto &group) { return !groups.contains(group.object_key); });
        for (auto &group : volume.sample_bank_groups) {
            std::erase_if(group.member_bank_keys, [&](const auto &key) { return !banks.contains(key); });
            std::erase_if(group.relationship_bank_keys, [&](const auto &key) { return !banks.contains(key); });
        }
        std::erase_if(volume.programs, [&](const auto &program) { return !programs.contains(program.object_key); });
    }
    std::erase_if(plan.volumes,
                  [](const auto &volume) { return volume.waveforms.empty() && volume.sample_banks.empty(); });
}
