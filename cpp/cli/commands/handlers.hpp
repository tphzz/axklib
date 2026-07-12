#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>

#include "requests.hpp"

namespace axk::cli::commands {

int run_info_request(const InfoRequest& request);
int run_objects_request(const ObjectsRequest& request);
int run_objects(const std::filesystem::path& path, bool pretty);
int run_relationships_request(const RelationshipsRequest& request);
int run_relationships(const std::filesystem::path& path, bool pretty);
int run_inventory_request(const InventoryRequest& request);
int run_coverage_request(const CoverageRequest& request);
int run_corpus_audit_request(const CorpusAuditRequest& request);
int run_extract_request(const ExtractRequest& request);
int run_tree(const std::filesystem::path& path, bool pretty, bool include_default_programs);
int run_orphans_request(const OrphansRequest& request);
int run_validate_request(const ValidateRequest& request);
int run_extract_wav(const std::filesystem::path& path,
                    const std::filesystem::path& output_directory, bool overwrite, bool pretty);
int run_export(const std::filesystem::path& path,
               const std::filesystem::path& output_directory, bool overwrite, bool write_sfz,
               bool pretty);
int run_preview(const std::filesystem::path& path, std::string_view object_key,
                std::size_t bins, bool pretty);
int run_create_hds(const std::filesystem::path& manifest_path,
                   const std::filesystem::path& output_path, bool overwrite, bool pretty);
int run_alter_hds(const std::filesystem::path& source_path,
                  const std::filesystem::path& manifest_path,
                  const std::optional<std::filesystem::path>& output_path, bool pretty);

}  // namespace axk::cli::commands
