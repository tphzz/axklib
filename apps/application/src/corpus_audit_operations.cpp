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

axk::app::Result<Json> execute_corpus_audit(const axk::app::Sandbox &sandbox, const Json &input,
                                            const axk::app::OperationContext &context) {
    const auto request = parse_corpus_audit_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.create_staging_directory("axklib-corpus-audit");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> manifest;
    std::vector<LoadedSource> loaded;
    std::size_t load_error_count{};
    loaded.reserve(request->sources.size());
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        const auto display = source_display_path(source_ref, context);
        const auto display_file = axk::text::path_from_utf8(display);
        auto suffix = display_file ? axk::text::path_to_utf8(display_file->extension()) : std::string{};
        std::ranges::transform(suffix, suffix.begin(),
                               [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const auto resolved = sandbox.resolve_file(source_ref);
        manifest.push_back({{"path", display},
                            {"exists", resolved.has_value()},
                            {"is_file", resolved.has_value()},
                            {"is_dir", false},
                            {"suffix", suffix}});
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_source(sandbox, source_ref, false, context, axk::MediaObjectReadMode::complete);
        if (!source) {
            ++load_error_count;
            continue;
        }
        loaded.push_back(std::move(*source));
    }

    std::vector<axk::ReportRow> inventory;
    std::vector<axk::ReportRow> relationships;
    std::vector<axk::ReportRow> validation_issues;
    std::vector<axk::ReportRow> wave_issues;
    std::uint64_t wave_decoded{};
    bool validation_failed{};
    std::uint64_t ambiguous{};
    for (const auto &source : loaded) {
        const auto display = source_display_path(source.source, context);
        for (const auto &item : source.inventory.catalog.objects)
            inventory.push_back(inventory_row(source, item, display));
        for (const auto &row : source.graph.relationships) {
            relationships.push_back(relationship_report_row(source, row, display));
            if (row.quality == axk::RelationshipQuality::tentative)
                ++ambiguous;
        }
        if (const auto *container = std::get_if<axk::Container>(&source.media.storage())) {
            const auto validation = axk::validate_semantics(*container, source.inventory.catalog, source.graph);
            validation_failed = validation_failed || !validation.valid();
            for (const auto &issue : validation.issues) {
                const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                                      : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                           : "info";
                validation_issues.push_back({{"severity", severity},
                                             {"code", issue.code},
                                             {"message", issue.message},
                                             {"scope", "relationship"},
                                             {"source_path", display},
                                             {"sampler_path", issue.sampler_path},
                                             {"object_key", issue.object_key},
                                             {"quality", "Known"},
                                             {"basis", "validation"},
                                             {"recommended_next_check", ""}});
            }
        }
        if (!request->skip_wave_smoke) {
            std::uint64_t successful{};
            for (const auto &item : source.inventory.catalog.objects) {
                if (item.object.header.type != axk::ObjectType::smpl)
                    continue;
                axk::Result<axk::Waveform> waveform =
                    source.media.kind() == axk::MediaKind::sfs
                        ? axk::decode_waveform(std::get<axk::Container>(source.media.storage()), item)
                        : [&]() -> axk::Result<axk::Waveform> {
                    const auto object =
                        std::ranges::find(source.inventory.objects, item.key, &axk::MediaObjectDescriptor::key);
                    if (object == source.inventory.objects.end()) {
                        return std::unexpected{axk::make_error(axk::ErrorCode::object_malformed,
                                                               axk::ErrorCategory::object,
                                                               "Wave Data object payload is unavailable")};
                    }
                    return axk::decode_waveform(item, object->logical_path);
                }();
                if (waveform) {
                    ++successful;
                } else {
                    wave_issues.push_back({{"source_path", display},
                                           {"container_kind", info_media_kind_name(source.media.kind())},
                                           {"object_key", public_object_key(source, item.key)},
                                           {"sample_name", item.object.header.name},
                                           {"code", static_cast<std::uint64_t>(waveform.error().code)},
                                           {"severity", "error"},
                                           {"message", waveform.error().message}});
                }
            }
            wave_decoded += std::min(successful, static_cast<std::uint64_t>(request->wave_smoke_limit));
        }
    }

    axk::ReportRow summary{
        {"input_count", static_cast<std::uint64_t>(request->sources.size())},
        {"loaded_container_count", static_cast<std::uint64_t>(loaded.size())},
        {"load_error_count", static_cast<std::uint64_t>(load_error_count)},
        {"relationship_load_error_count", static_cast<std::uint64_t>(load_error_count)},
        {"object_count", static_cast<std::uint64_t>(inventory.size())},
        {"validation_issue_count", static_cast<std::uint64_t>(validation_issues.size())},
        {"validation_failed", validation_failed},
        {"relationship_count", static_cast<std::uint64_t>(relationships.size())},
        {"ambiguous_relationship_count", ambiguous},
        {"wave_smoke_decoded", wave_decoded},
        {"wave_smoke_errors", static_cast<std::uint64_t>(wave_issues.size())},
    };
    if (auto written =
            axk::write_report_object(*destination / "corpus_audit_summary.json", summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "corpus_audit_summary.json")}));
    }
    if (auto written = axk::write_report_json(*destination / "input_manifest.json", manifest, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "input_manifest.json")}));
    }
    std::vector<axk::ReportSchemaManifest> schemas;
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    const std::array summary_rows{summary};
    auto summary_schema = axk::make_report_schema("corpus_audit_summary", summary_rows, options);
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "corpus_audit_summary.schema.json",
                                                summary_schema, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(),
                       {request->destination.root_id,
                        child_reference_path(request->destination, "_schemas/corpus_audit_summary.schema.json")}));
    }
    schemas.push_back(std::move(summary_schema));
    for (const auto &[name, rows] :
         std::initializer_list<std::pair<std::string_view, const std::vector<axk::ReportRow> &>>{
             {"input_manifest", manifest},
             {"inventory_objects", inventory},
             {"validation_issues", validation_issues},
             {"relationships", relationships},
             {"wave_smoke_issues", wave_issues}}) {
        auto schema = write_csv_schema(*destination, request->destination, std::string{name}, rows, request->overwrite);
        if (!schema)
            return std::unexpected(schema.error());
        schemas.push_back(std::move(*schema));
    }
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }

    auto artifacts = Json::array();
    for (const auto path : {"corpus_audit_summary.json", "input_manifest.csv", "input_manifest.json",
                            "inventory_objects.csv", "validation_issues.csv", "relationships.csv",
                            "wave_smoke_issues.csv", "_schemas/corpus_audit_summary.schema.json",
                            "_schemas/input_manifest.schema.json", "_schemas/inventory_objects.schema.json",
                            "_schemas/validation_issues.schema.json", "_schemas/relationships.schema.json",
                            "_schemas/wave_smoke_issues.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (context.progress != nullptr) {
        context.progress->report({axk::ProgressPhase::writing, request->sources.size(), request->sources.size(),
                                  "corpus audit", std::nullopt});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    return Json{{"operationId", "corpus.audit"},         {"sourceCount", request->sources.size()},
                {"loadedCount", loaded.size()},          {"failedCount", load_error_count},
                {"objectCount", inventory.size()},       {"validationIssueCount", validation_issues.size()},
                {"validationFailed", validation_failed}, {"relationshipCount", relationships.size()},
                {"waveSmokeDecoded", wave_decoded},      {"waveSmokeErrorCount", wave_issues.size()},
                {"artifacts", std::move(artifacts)}};
}

} // namespace axk::app::file_operations_internal
