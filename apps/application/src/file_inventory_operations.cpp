#include "axklib/application/file_operations.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/terminology.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

#include "file_operations_internal.hpp"

namespace axk::app::file_operations_internal {

std::string child_reference_path(const axk::app::DirectoryRef &directory, std::string_view child) {
    return directory.relative_path.empty() ? std::string{child} : std::format("{}/{}", directory.relative_path, child);
}

axk::app::Result<axk::ReportSchemaManifest> write_report_set(const std::filesystem::path &destination,
                                                             const axk::app::DirectoryRef &destination_ref,
                                                             std::string name, std::span<const axk::ReportRow> rows,
                                                             std::string semantic_notes, bool overwrite) {
    const auto csv_name = name + ".csv";
    if (auto written = axk::write_report_csv(destination / csv_name, rows, {}, overwrite); !written)
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, csv_name)}));
    const auto json_name = name + ".json";
    if (auto written = axk::write_report_json(destination / json_name, rows, overwrite); !written)
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, json_name)}));
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    options.semantic_notes = std::move(semantic_notes);
    auto schema = axk::make_report_schema(name, rows, std::move(options));
    const auto schema_name = std::format("_schemas/{}.schema.json", name);
    if (auto written = axk::write_report_schema(destination / schema_name, schema, overwrite); !written)
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, schema_name)}));
    return schema;
}

axk::app::Result<axk::ReportSchemaManifest> write_csv_schema(const std::filesystem::path &destination,
                                                             const axk::app::DirectoryRef &destination_ref,
                                                             std::string name, std::span<const axk::ReportRow> rows,
                                                             bool overwrite) {
    const auto csv_name = name + ".csv";
    if (auto written = axk::write_report_csv(destination / csv_name, rows, {}, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, csv_name)}));
    }
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    auto schema = axk::make_report_schema(name, rows, options);
    const auto schema_name = std::format("_schemas/{}.schema.json", name);
    if (auto written = axk::write_report_schema(destination / schema_name, schema, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, schema_name)}));
    }
    return schema;
}

axk::app::Result<Json> execute_inventory(const axk::app::Sandbox &sandbox, const Json &input,
                                         const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.create_staging_directory("axklib-report-inventory");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> objects;
    std::vector<axk::ReportRow> issues;
    std::vector<axk::ReportRow> load_errors;
    std::map<std::string, std::uint64_t> counts;
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_info_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source) {
            if (request->strict)
                return std::unexpected(source.error().error);
            load_errors.push_back({{"path", source_display_path(source_ref, context)},
                                   {"error_code", source.error().error_code},
                                   {"message", source.error().error.message},
                                   {"recoverable", true},
                                   {"original_exception", source.error().original_exception}});
            continue;
        }
        const auto display_path = source_display_path(source_ref, context);
        for (const auto &item : source->inventory.catalog.objects) {
            objects.push_back(inventory_row(*source, item, display_path));
            ++counts[object_type_name(item.object.header.type)];
        }
        for (const auto &issue : source->inventory.catalog.issues) {
            issues.push_back(
                {{"source_path", display_path},
                 {"container_kind", info_media_kind_name(source->media.kind())},
                 {"object_key",
                  issue.sfs_id ? std::format("p{}:sfs{}", issue.partition.value, issue.sfs_id->value) : std::string{}},
                 {"object_type", ""},
                 {"object_name", ""},
                 {"code", issue.code},
                 {"severity", "error"},
                 {"message", issue.message},
                 {"byte_start", nullptr},
                 {"byte_end", nullptr},
                 {"quality", "Unknown"},
                 {"basis", "native catalog decode"}});
        }
    }

    auto object_schema =
        write_report_set(*destination, request->destination, "inventory_objects", objects,
                         "Decoded object inventory rows produced through axklib.objects.decoded.", request->overwrite);
    if (!object_schema)
        return std::unexpected(object_schema.error());
    auto issue_schema = write_report_set(*destination, request->destination, "decode_issues", issues,
                                         "Decode issues use stable code/severity/quality fields.", request->overwrite);
    if (!issue_schema)
        return std::unexpected(issue_schema.error());

    axk::ReportValue::Object type_counts;
    for (const auto &[name, count] : counts)
        type_counts.emplace_back(name, count);
    axk::ReportValue::Array serialized_load_errors;
    for (const auto &row : load_errors)
        serialized_load_errors.emplace_back(axk::ReportValue::Object{row.begin(), row.end()});
    axk::ReportRow summary{{"input_count", static_cast<std::uint64_t>(request->sources.size())},
                           {"object_count", static_cast<std::uint64_t>(objects.size())},
                           {"decode_issue_count", static_cast<std::uint64_t>(issues.size())},
                           {"load_error_count", static_cast<std::uint64_t>(load_errors.size())},
                           {"object_type_counts", std::move(type_counts)},
                           {"load_errors", std::move(serialized_load_errors)}};
    const auto summary_name = std::string{"inventory_summary.json"};
    if (auto written = axk::write_report_object(*destination / summary_name, summary, request->overwrite); !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id, child_reference_path(request->destination, summary_name)}));
    }
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    const auto summary_schema = axk::make_report_schema("inventory_summary", std::span{&summary, 1U}, summary_options);
    const auto summary_schema_name = std::string{"_schemas/inventory_summary.schema.json"};
    if (auto written = axk::write_report_schema(*destination / summary_schema_name, summary_schema, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, summary_schema_name)}));
    }
    const std::array schemas{*object_schema, *issue_schema, summary_schema};
    const auto index_name = std::string{"_schemas/schema_index.json"};
    if (auto written = axk::write_report_schema_index(*destination / index_name, schemas, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id, child_reference_path(request->destination, index_name)}));
    }

    auto artifacts = Json::array();
    for (const auto path :
         {"inventory_objects.csv", "inventory_objects.json", "decode_issues.csv", "decode_issues.json",
          "inventory_summary.json", "_schemas/inventory_objects.schema.json", "_schemas/decode_issues.schema.json",
          "_schemas/inventory_summary.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (context.progress != nullptr) {
        context.progress->report(
            {axk::ProgressPhase::writing, request->sources.size(), request->sources.size(), "inventory", std::nullopt});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    return Json{{"operationId", "report.inventory"},
                {"sourceCount", request->sources.size()},
                {"loadedCount", request->sources.size() - load_errors.size()},
                {"failedCount", load_errors.size()},
                {"rowCount", objects.size()},
                {"decodeIssueCount", issues.size()},
                {"artifacts", std::move(artifacts)}};
}

} // namespace axk::app::file_operations_internal
