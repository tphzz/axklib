#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/file_operations.hpp"
#include "axklib/media.hpp"
#include "axklib/report.hpp"

namespace axk::app::file_operations_internal {

using Json = nlohmann::json;

struct ReportRequest {
    std::vector<FileRef> sources;
    DirectoryRef destination;
    bool overwrite{};
    bool strict{};
    bool include_default_programs{};
    std::optional<std::string> object_type;
};

struct InfoRequest {
    std::vector<ImageSourceRef> sources;
    bool strict{};
    bool include_default_programs{};
};

struct CorpusAuditRequest {
    std::vector<FileRef> sources;
    DirectoryRef destination;
    std::string policy{"normal"};
    std::size_t wave_smoke_limit{10U};
    bool skip_wave_smoke{};
    bool overwrite{};
};

struct InfoLoadFailure {
    Error error;
    std::uint64_t error_code{static_cast<std::uint64_t>(axk::ErrorCode::io_open_failed)};
    std::string original_exception{"axk::Error"};
};

struct LoadedSource {
    ImageSourceRef source;
    axk::MediaContainer media;
    axk::MediaInventory inventory;
    axk::RelationshipGraph graph;
    axk::ContentTree tree;
};

struct DirectoryCleanup {
    std::filesystem::path path;

    ~DirectoryCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

Error operation_error(std::string code, std::string message, std::optional<std::string> relative_path = std::nullopt);
Error core_error(const axk::Error &error, const FileRef &source);
Error image_source_error(const axk::Error &error, const ImageSourceRef &source);
Result<ReportRequest> parse_request(const Json &input);
Result<InfoRequest> parse_info_request(const Json &input);
Result<CorpusAuditRequest> parse_corpus_audit_request(const Json &input);
std::string info_media_kind_name(axk::MediaKind kind);
std::string object_type_name(axk::ObjectType type);
Result<LoadedSource> load_source(const Sandbox &sandbox, const FileRef &source, bool include_default_programs,
                                 const OperationContext &context,
                                 axk::MediaObjectReadMode read_mode = axk::MediaObjectReadMode::decoded_metadata);
std::expected<LoadedSource, InfoLoadFailure> load_info_source(const Sandbox &sandbox, const ImageSourceRef &source,
                                                              bool include_default_programs,
                                                              const OperationContext &context);
std::expected<LoadedSource, InfoLoadFailure> load_info_source(const Sandbox &sandbox, const FileRef &source,
                                                              bool include_default_programs,
                                                              const OperationContext &context);
std::string source_display_path(const FileRef &source, const OperationContext &context);
std::string source_display_path(const ImageSourceRef &source, const OperationContext &context);
std::string source_filename(const LoadedSource &source);
std::string public_object_key(const LoadedSource &source, std::string_view native_key);
std::string public_scope_key(const LoadedSource &source, const axk::ObjectSnapshot &item,
                             std::string_view display_path);
axk::ReportRow inventory_row(const LoadedSource &source, const axk::ObjectSnapshot &item, std::string display_path);
Json info_tree_json(const LoadedSource &source, std::string display_path);
std::string child_reference_path(const DirectoryRef &directory, std::string_view child);
Result<axk::ReportSchemaManifest> write_report_set(const std::filesystem::path &destination,
                                                   const DirectoryRef &destination_ref, std::string name,
                                                   std::span<const axk::ReportRow> rows, std::string semantic_notes,
                                                   bool overwrite);
Result<axk::ReportSchemaManifest> write_csv_schema(const std::filesystem::path &destination,
                                                   const DirectoryRef &destination_ref, std::string name,
                                                   std::span<const axk::ReportRow> rows, bool overwrite);

std::string joined_strings(const std::vector<std::string> &items);
axk::ReportRow relationship_report_row(const LoadedSource &source, const axk::Relationship &row,
                                       std::string_view display_path);
std::size_t program_ignored_count(const LoadedSource &source);
const axk::ObjectSnapshot *catalog_object(const LoadedSource &source, std::string_view key);
const axk::MediaObjectDescriptor *media_object(const LoadedSource &source, std::string_view key);
const axk::FatFile *fat_file_metadata(const LoadedSource &source, const axk::MediaObjectDescriptor *object);
std::uint64_t sfs_payload_offset(const LoadedSource &source, const axk::ObjectSnapshot &item);
std::string joined_programs(const std::vector<std::uint8_t> &items);
axk::ReportValue optional_unsigned(bool present, std::uint64_t value);
std::vector<axk::ReportRow> sbac_detail_rows(std::span<const LoadedSource> sources, const OperationContext &context);
std::vector<axk::ReportRow> bitmap_detail_rows(std::span<const LoadedSource> sources, const OperationContext &context);
std::vector<axk::ReportRow> program_detail_rows(std::span<const LoadedSource> sources, const OperationContext &context);
std::vector<axk::ReportRow> program_ignored_detail_rows(std::span<const LoadedSource> sources,
                                                        const OperationContext &context);
axk::ReportRow coverage_summary(const std::vector<LoadedSource> &sources, std::span<const axk::ReportRow> relationships,
                                std::size_t load_error_count);

Result<Json> execute_objects(const Sandbox &sandbox, const Json &input, const OperationContext &context);
Result<Json> execute_info(const Sandbox &sandbox, const Json &input, const OperationContext &context);
Result<Json> execute_inventory(const Sandbox &sandbox, const Json &input, const OperationContext &context);
Result<Json> execute_coverage(const Sandbox &sandbox, const Json &input, const OperationContext &context);
Result<Json> execute_orphans(const Sandbox &sandbox, const Json &input, const OperationContext &context);
Result<Json> execute_relationships(const Sandbox &sandbox, const Json &input, const OperationContext &context);
Result<Json> execute_corpus_audit(const Sandbox &sandbox, const Json &input, const OperationContext &context);

} // namespace axk::app::file_operations_internal
