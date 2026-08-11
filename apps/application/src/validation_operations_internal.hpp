#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/validation_operations.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"

namespace axk::app::validation_operations_internal {

using Json = nlohmann::json;

struct ValidationRequest {
    std::vector<FileRef> sources;
    std::optional<DirectoryRef> exports;
    DirectoryRef destination;
    std::string policy{"normal"};
    bool overwrite{};
};

struct ValidationSource {
    FileRef reference;
    std::filesystem::path path;
    axk::MediaContainer media;
    std::vector<axk::MediaObjectDescriptor> objects;
    axk::ObjectCatalog catalog;
    axk::RelationshipGraph graph;
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
Result<ValidationRequest> parse_request(const Json &input);
std::string display_path(const FileRef &source, const OperationContext &context);
Result<ValidationSource> load_source(const Sandbox &sandbox, const FileRef &source, const OperationContext &context);
std::string child_reference_path(const DirectoryRef &directory, std::string_view child);

std::vector<axk::ReportRow> allocation_summary_rows(const std::filesystem::path &path, const axk::Container &container);
std::vector<axk::ReportRow> allocation_extent_rows(const std::filesystem::path &path, const axk::Container &container);
std::vector<axk::ReportRow> allocation_mismatch_rows(const std::filesystem::path &path,
                                                     std::span<const axk::Partition> partitions);
std::vector<axk::ReportRow> volume_validation_rows(const std::filesystem::path &path, const axk::Container &container,
                                                   const axk::ObjectCatalog &catalog,
                                                   std::vector<axk::ReportRow> &issue_rows,
                                                   std::vector<axk::ReportRow> &validation_issues);
std::vector<axk::ReportRow> validate_media_details(const ValidationSource &source, bool include_object_checks = true);
std::vector<axk::ReportRow> validate_export_directory(const SandboxTree &tree);

Result<axk::ReportSchemaManifest> write_report_set(const std::filesystem::path &destination,
                                                   const DirectoryRef &destination_reference, const std::string &name,
                                                   std::span<const axk::ReportRow> rows, bool overwrite);
Result<Json> execute_validation(const Sandbox &sandbox, const Json &input, const OperationContext &context);

} // namespace axk::app::validation_operations_internal
