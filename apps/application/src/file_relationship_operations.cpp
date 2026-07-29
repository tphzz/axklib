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

axk::app::Result<Json> execute_orphans(const axk::app::Sandbox &sandbox, const Json &input,
                                       const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.create_staging_directory("axklib-report-orphans");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> rows;
    std::vector<axk::ReportRow> summaries;
    auto response_summaries = Json::array();
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source)
            return std::unexpected(source.error());
        const auto *container = std::get_if<axk::Container>(&source->media.storage());
        if (container == nullptr) {
            return std::unexpected(operation_error("unsupported_media", "Wave Data orphan analysis requires SFS media",
                                                   source_ref.relative_path));
        }
        const auto report = axk::analyze_waveform_orphans(*container, source->inventory.catalog, source->graph);
        const auto display_path = source_display_path(source_ref, context);
        for (const auto &row : report.rows) {
            rows.push_back({
                {"source_path", display_path},
                {"partition_index", static_cast<std::uint64_t>(row.partition.value)},
                {"partition_name", row.partition_name},
                {"volume_name", row.volume_name},
                {"waveform_name", row.waveform_name},
                {"object_key", row.object_key},
                {"sfs_id", static_cast<std::uint64_t>(row.sfs_id.value)},
                {"wave_data_reference_value", static_cast<std::uint64_t>(row.wave_data_reference_value)},
                {"status", std::string{axk::waveform_status_name(row.status)}},
                {"referencing_samples", joined_strings(row.referencing_samples)},
                {"basis", row.basis},
                {"notes", row.notes},
            });
        }
        summaries.push_back({
            {"source_path", display_path},
            {"waveform_count", static_cast<std::uint64_t>(report.rows.size())},
            {"referenced_count", static_cast<std::uint64_t>(report.referenced_count)},
            {"known_unreferenced_count", static_cast<std::uint64_t>(report.known_unreferenced_count)},
            {"ambiguous_or_unresolved_count", static_cast<std::uint64_t>(report.ambiguous_or_unresolved_count)},
        });
        response_summaries.push_back({{"sourcePath", display_path},
                                      {"waveformCount", report.rows.size()},
                                      {"referencedCount", report.referenced_count},
                                      {"knownUnreferencedCount", report.known_unreferenced_count},
                                      {"ambiguousOrUnresolvedCount", report.ambiguous_or_unresolved_count}});
    }
    auto row_schema =
        write_report_set(*destination, request->destination, "waveform_orphans", rows, {}, request->overwrite);
    if (!row_schema)
        return std::unexpected(row_schema.error());
    auto summary_schema = write_report_set(*destination, request->destination, "waveform_orphan_summary", summaries, {},
                                           request->overwrite);
    if (!summary_schema)
        return std::unexpected(summary_schema.error());
    const std::array schemas{*row_schema, *summary_schema};
    const auto index_name = std::string{"_schemas/schema_index.json"};
    if (auto written = axk::write_report_schema_index(*destination / index_name, schemas, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id, child_reference_path(request->destination, index_name)}));
    }
    auto artifacts = Json::array();
    for (const auto path : {"waveform_orphans.csv", "waveform_orphans.json", "waveform_orphan_summary.csv",
                            "waveform_orphan_summary.json", "_schemas/waveform_orphans.schema.json",
                            "_schemas/waveform_orphan_summary.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (context.progress != nullptr) {
        context.progress->report(
            {axk::ProgressPhase::writing, request->sources.size(), request->sources.size(), "orphans", std::nullopt});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    return Json{{"operationId", "report.orphans"},
                {"sourceCount", request->sources.size()},
                {"loadedCount", request->sources.size()},
                {"failedCount", 0U},
                {"rowCount", rows.size()},
                {"summaries", std::move(response_summaries)},
                {"artifacts", std::move(artifacts)}};
}

axk::app::Result<Json> execute_relationships(const axk::app::Sandbox &sandbox, const Json &input,
                                             const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.create_staging_directory("axklib-report-relationships");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }
    std::vector<LoadedSource> loaded;
    std::vector<axk::ReportRow> load_errors;
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_info_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source) {
            load_errors.push_back({{"path", source_display_path(source_ref, context)},
                                   {"error_code", source.error().error_code},
                                   {"message", source.error().error.message},
                                   {"recoverable", true},
                                   {"original_exception", source.error().original_exception}});
            continue;
        }
        loaded.push_back(std::move(*source));
    }
    std::vector<axk::ReportRow> rows;
    std::size_t ambiguous_count{};
    for (const auto &source : loaded) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &row : source.graph.relationships) {
            rows.push_back(relationship_report_row(source, row, display_path));
            if (row.quality == axk::RelationshipQuality::tentative)
                ++ambiguous_count;
        }
    }
    const auto sbac_rows = sbac_detail_rows(loaded, context);
    const auto program_rows = program_detail_rows(loaded, context);
    const auto ignored_rows = program_ignored_detail_rows(loaded, context);
    const auto bitmap_rows = bitmap_detail_rows(loaded, context);

    std::vector<axk::ReportSchemaManifest> schemas;
    const auto append_report = [&](std::string name,
                                   std::span<const axk::ReportRow> report_rows) -> axk::app::Result<void> {
        auto schema =
            write_report_set(*destination, request->destination, std::move(name), report_rows, {}, request->overwrite);
        if (!schema)
            return std::unexpected(schema.error());
        schemas.push_back(std::move(*schema));
        return {};
    };
    if (auto written = append_report("relationships", rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_sbac_sbnk_links", sbac_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_prog_assignment_links", program_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_prog_ignored_reserved_or_tail", ignored_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_sbnk_program_bitmap_crosscheck", bitmap_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("load_errors", load_errors); !written)
        return std::unexpected(written.error());

    auto summary = coverage_summary(loaded, rows, load_errors.size());
    if (auto written =
            axk::write_report_object(*destination / "relationship_summary.json", summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "relationship_summary.json")}));
    }
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    auto summary_schema =
        axk::make_report_schema("relationship_summary", std::span{&summary, 1U}, std::move(summary_options));
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "relationship_summary.schema.json",
                                                summary_schema, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(),
                       {request->destination.root_id,
                        child_reference_path(request->destination, "_schemas/relationship_summary.schema.json")}));
    }
    schemas.push_back(std::move(summary_schema));
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }
    auto artifacts = Json::array();
    for (const auto path : {
             "relationships.csv",
             "relationships.json",
             "current_sbac_sbnk_links.csv",
             "current_sbac_sbnk_links.json",
             "current_prog_assignment_links.csv",
             "current_prog_assignment_links.json",
             "current_prog_ignored_reserved_or_tail.csv",
             "current_prog_ignored_reserved_or_tail.json",
             "current_sbnk_program_bitmap_crosscheck.csv",
             "current_sbnk_program_bitmap_crosscheck.json",
             "load_errors.csv",
             "load_errors.json",
             "relationship_summary.json",
             "_schemas/relationships.schema.json",
             "_schemas/current_sbac_sbnk_links.schema.json",
             "_schemas/current_prog_assignment_links.schema.json",
             "_schemas/current_prog_ignored_reserved_or_tail.schema.json",
             "_schemas/current_sbnk_program_bitmap_crosscheck.schema.json",
             "_schemas/load_errors.schema.json",
             "_schemas/relationship_summary.schema.json",
             "_schemas/schema_index.json",
         }) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    return Json{{"operationId", "report.relationships"},
                {"sourceCount", request->sources.size()},
                {"loadedCount", loaded.size()},
                {"failedCount", load_errors.size()},
                {"rowCount", rows.size()},
                {"ambiguousCount", ambiguous_count},
                {"artifacts", std::move(artifacts)}};
}

} // namespace axk::app::file_operations_internal
