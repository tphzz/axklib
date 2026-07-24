#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/catalog.hpp"
#include "axklib/export.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"

namespace axk {

class MediaContainer;

struct ContentNode {
    std::string node_id;
    std::string node_type;
    std::string display_name;
    std::string object_key;
    std::string object_type;
    RelationshipQuality quality{RelationshipQuality::known};
    std::string basis;
    std::string notes;
    std::vector<std::string> details;
    std::vector<ContentNode> children;

    ContentNode() = default;
    ContentNode(std::string id, std::string type, std::string display, std::string key = {},
                std::string stored_object_type = {}, RelationshipQuality stored_quality = RelationshipQuality::known,
                std::string stored_basis = {}, std::string stored_notes = {},
                std::vector<std::string> stored_details = {}, std::vector<ContentNode> stored_children = {})
        : node_id(std::move(id)), node_type(std::move(type)), display_name(std::move(display)),
          object_key(std::move(key)), object_type(std::move(stored_object_type)), quality(stored_quality),
          basis(std::move(stored_basis)), notes(std::move(stored_notes)), details(std::move(stored_details)),
          children(std::move(stored_children)) {}
};

struct ContentTreeIssue {
    std::string code;
    std::string severity;
    std::string message;
    std::string sampler_path;
    std::string object_key;
};

struct ContentTree {
    std::string source_path;
    std::vector<ContentNode> roots;
    std::vector<ContentTreeIssue> issues;
};

enum class WaveformStatus : std::uint8_t {
    referenced,
    known_unreferenced,
    ambiguous_or_unresolved,
};

struct WaveformOrphanRow {
    PartitionIndex partition;
    std::string partition_name;
    std::string volume_name;
    std::string waveform_name;
    std::string object_key;
    SfsId sfs_id;
    std::uint32_t wave_data_reference_value{};
    WaveformStatus status{WaveformStatus::ambiguous_or_unresolved};
    std::vector<std::string> referencing_samples;
    std::string basis;
    std::string notes;
};

struct WaveformOrphanReport {
    std::vector<WaveformOrphanRow> rows;
    std::size_t referenced_count{};
    std::size_t known_unreferenced_count{};
    std::size_t ambiguous_or_unresolved_count{};
};

enum class ValidationSeverity : std::uint8_t { info, warning, error };

struct ValidationIssue {
    std::string code;
    ValidationSeverity severity{ValidationSeverity::error};
    std::string message;
    std::string sampler_path;
    std::string object_key;
};

struct CoverageSummary {
    std::size_t object_count{};
    std::size_t relationship_count{};
    std::size_t known_relationship_count{};
    std::size_t likely_relationship_count{};
    std::size_t tentative_relationship_count{};
    std::size_t unknown_relationship_count{};
    std::size_t exact_placement_count{};
    std::size_t unresolved_placement_count{};
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    CoverageSummary coverage;

    [[nodiscard]] AXK_API bool valid() const noexcept;
};

AXK_API ContentTree build_content_tree(const Container &container, const ObjectCatalog &catalog,
                                       const RelationshipGraph &graph, bool include_default_programs = false);
AXK_API ContentTree build_content_tree(const MediaContainer &container, const ObjectCatalog &catalog,
                                       const RelationshipGraph &graph, bool include_default_programs = false);
AXK_API ContentTree build_content_tree(std::string source_path, const ObjectCatalog &catalog,
                                       const RelationshipGraph &graph, bool include_default_programs = false);
AXK_API WaveformOrphanReport analyze_waveform_orphans(const Container &container, const ObjectCatalog &catalog,
                                                      const RelationshipGraph &graph);
AXK_API ValidationReport validate_semantics(const Container &container, const ObjectCatalog &catalog,
                                            const RelationshipGraph &graph);
AXK_API std::string_view waveform_status_name(WaveformStatus status) noexcept;

} // namespace axk
