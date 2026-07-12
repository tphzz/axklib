#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/catalog.hpp"
#include "axklib/error.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"

namespace axk::cli::commands {

inline constexpr std::string_view oracle_report_library_version{"0.1.0-plan008"};

struct CliLoaded {
  std::filesystem::path path;
  MediaContainer media;
  std::vector<MediaObject> objects;
  ObjectCatalog catalog;
  RelationshipGraph graph;
};

struct SemanticSnapshot {
  Container container;
  ObjectCatalog catalog;
  RelationshipGraph graph;
};

struct CliLoadResult {
  std::vector<CliLoaded> loaded;
  std::vector<ReportRow> errors;
};

std::string object_type_text(ObjectType type);
std::string media_kind_text(MediaKind kind);
std::vector<std::filesystem::path> expand_cli_paths(
    const std::vector<std::filesystem::path>& inputs);
CliLoadResult load_cli_paths(const std::vector<std::filesystem::path>& inputs);
Result<SemanticSnapshot> load_semantic_snapshot(const std::filesystem::path& path);
Result<void> prepare_report_directory(const std::filesystem::path& path, bool overwrite);
Result<ReportSchemaManifest> write_cli_report(const std::filesystem::path& output,
                                              std::string name,
                                              std::span<const ReportRow> rows,
                                              std::string source_command, bool overwrite);
std::string public_object_key(const CliLoaded& loaded, std::string_view native_key);
std::string public_scope_key(const CliLoaded& loaded, const ObjectSnapshot& item);
ReportRow inventory_row(const CliLoaded& loaded, const ObjectSnapshot& item);
ReportRow relationship_report_row(const CliLoaded& loaded, const Relationship& row);
ReportRow coverage_summary(const CliLoadResult& loaded, std::span<const ReportRow> relationships);
int report_failure(const Error& error);
ContentTree cli_content_tree(const CliLoaded& loaded, bool include_default_programs);
std::string sfs_selector_component(const ContentNode& node);

}  // namespace axk::cli::commands
