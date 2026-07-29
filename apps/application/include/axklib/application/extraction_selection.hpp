#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/media.hpp"
#include "axklib/semantic.hpp"

namespace axk::app {

struct ExtractionSelection {
    std::string object_key;
};

struct ExcludedExtractionRelationship {
    std::string source_key;
    std::optional<std::string> target_key;
    std::vector<std::string> candidate_keys;
    std::string type;
    RelationshipQuality quality{RelationshipQuality::unknown};
    std::string basis;
    AssignmentState assignment_state{AssignmentState::unknown};
    std::string reason;
};

struct ExactExportClosure {
    std::set<std::string> programs;
    std::set<std::string> sample_banks;
    std::set<std::string> samples;
    std::set<std::string> wave_data;
    std::set<std::pair<std::string, std::string>> program_targets;
    std::set<std::pair<std::string, std::string>> sample_bank_members;
    std::set<std::pair<std::string, std::string>> sample_wave_data;
    std::vector<ExcludedExtractionRelationship> excluded;
};

[[nodiscard]] Result<ExtractionSelection> resolve_extraction_selection(MediaKind media_kind, const ContentTree &tree,
                                                                       std::string_view scope,
                                                                       std::string_view selector_path);

[[nodiscard]] ExactExportClosure build_exact_export_closure(const RelationshipGraph &graph,
                                                            std::set<std::string> programs,
                                                            std::set<std::string> sample_banks,
                                                            std::set<std::string> samples,
                                                            std::set<std::string> wave_data);

void filter_export_plan(ExportPlan &plan, const ExactExportClosure &closure,
                        const std::set<std::pair<std::uint8_t, std::string>> &whole_volumes = {});

std::vector<ExcludedExtractionRelationship> filter_export_plan(ExportPlan &plan, const RelationshipGraph &graph,
                                                               std::string_view scope, std::string_view selector_path,
                                                               std::string_view selector_key);

} // namespace axk::app
