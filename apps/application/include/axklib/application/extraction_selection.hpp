#pragma once

#include <string>
#include <string_view>
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
    std::string target_key;
    std::string type;
};

[[nodiscard]] Result<ExtractionSelection> resolve_extraction_selection(MediaKind media_kind, const ContentTree &tree,
                                                                       std::string_view scope,
                                                                       std::string_view selector_path);

std::vector<ExcludedExtractionRelationship> filter_export_plan(ExportPlan &plan, const RelationshipGraph &graph,
                                                               std::string_view scope, std::string_view selector_path,
                                                               std::string_view selector_key);

} // namespace axk::app
