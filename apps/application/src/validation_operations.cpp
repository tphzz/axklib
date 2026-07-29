#include "axklib/application/validation_operations.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "validation_operations_internal.hpp"

namespace axk::app::validation_operations_internal {

Result<axk::ReportSchemaManifest> write_report_set(const std::filesystem::path &destination,
                                                   const DirectoryRef &destination_ref, const std::string &name,
                                                   std::span<const axk::ReportRow> rows, bool overwrite) {
    const auto csv_name = name + ".csv";
    if (auto written = axk::write_report_csv(destination / csv_name, rows, {}, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, csv_name)}));
    }
    const auto json_name = name + ".json";
    if (auto written = axk::write_report_json(destination / json_name, rows, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, json_name)}));
    }
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    if (name == "validation_issues")
        options.semantic_notes = "Validation issues use stable issue codes intended for regression and CI gates.";
    auto schema = axk::make_report_schema(name, rows, std::move(options));
    const auto schema_name = std::format("_schemas/{}.schema.json", name);
    if (auto written = axk::write_report_schema(destination / schema_name, schema, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, schema_name)}));
    }
    return schema;
}

Result<Json> execute_validation(const Sandbox &sandbox, const Json &input, const OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.create_staging_directory("axklib-validation");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> issues;
    std::vector<axk::ReportRow> allocation_summaries;
    std::vector<axk::ReportRow> allocation_extents;
    std::vector<axk::ReportRow> allocation_mismatches;
    std::vector<axk::ReportRow> volumes;
    std::vector<axk::ReportRow> volume_issues;
    std::map<std::string, std::uint64_t> issue_counts;
    bool failed{};
    bool has_sfs_input{};
    std::vector<ValidationSource> loaded;

    if (request->exports) {
        constexpr std::size_t maximum_export_files = 100'000U;
        constexpr std::uint64_t maximum_export_bytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
        auto export_files = sandbox.open_tree(*request->exports,
                                              {maximum_export_files, maximum_export_bytes, 64U, 32U * 1024U * 1024U});
        if (!export_files)
            return std::unexpected(export_files.error());
        issues = validate_export_directory(*export_files);
        for (const auto &issue : issues) {
            const auto code = std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
            if (code != issue.end())
                ++issue_counts[std::get<std::string>(code->second.value)];
        }
        failed = !issues.empty();
    } else {
        loaded.reserve(request->sources.size());
        for (std::size_t index = 0; index < request->sources.size(); ++index) {
            if (context.progress != nullptr) {
                context.progress->report({axk::ProgressPhase::reading, index, request->sources.size(),
                                          request->sources[index].relative_path, std::nullopt});
            }
            auto source = load_source(sandbox, request->sources[index], context);
            if (!source) {
                return std::unexpected(operation_error("validation_input_failed",
                                                       "one or more validation inputs could not be opened",
                                                       request->sources[index].relative_path));
            }
            loaded.push_back(std::move(*source));
        }
    }

    for (const auto &source : loaded) {
        if (source.media.kind() != axk::MediaKind::sfs) {
            auto source_issues = validate_media_details(source);
            for (auto &issue : source_issues) {
                const auto code = std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
                if (code != issue.end())
                    ++issue_counts[std::get<std::string>(code->second.value)];
                const auto severity =
                    std::ranges::find(issue, "severity", &std::pair<std::string, axk::ReportValue>::first);
                if (severity != issue.end()) {
                    const auto &value = std::get<std::string>(severity->second.value);
                    if (value == "error" || value == "fatal" || (request->policy == "strict" && value == "warning"))
                        failed = true;
                }
                issues.push_back(std::move(issue));
            }
            continue;
        }
        has_sfs_input = true;
        const auto &container = std::get<axk::Container>(source.media.storage());
        const auto validation = axk::validate_semantics(container, source.catalog, source.graph);
        for (const auto &issue : validation.issues) {
            if (issue.code.starts_with("REL_"))
                continue;
            const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                                  : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                       : "info";
            ++issue_counts[issue.code];
            if (issue.severity == axk::ValidationSeverity::error ||
                (request->policy == "strict" && issue.severity == axk::ValidationSeverity::warning))
                failed = true;
            issues.push_back({{"severity", severity},
                              {"code", issue.code},
                              {"message", issue.message},
                              {"scope", "relationship"},
                              {"source_path", axk::text::path_to_utf8(source.path)},
                              {"sampler_path", issue.sampler_path},
                              {"object_key", issue.object_key},
                              {"quality", "Known"},
                              {"basis", "validation"},
                              {"recommended_next_check", ""}});
        }
        auto relationship_issues = validate_media_details(source, false);
        for (auto &issue : relationship_issues) {
            const auto code = std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
            if (code != issue.end())
                ++issue_counts[std::get<std::string>(code->second.value)];
            const auto severity =
                std::ranges::find(issue, "severity", &std::pair<std::string, axk::ReportValue>::first);
            if (severity != issue.end()) {
                const auto &value = std::get<std::string>(severity->second.value);
                if (value == "error" || value == "fatal" || (request->policy == "strict" && value == "warning"))
                    failed = true;
            }
            issues.push_back(std::move(issue));
        }
        auto source_summaries = allocation_summary_rows(source.path, container);
        std::ranges::move(source_summaries, std::back_inserter(allocation_summaries));
        auto source_extents = allocation_extent_rows(source.path, container);
        std::ranges::move(source_extents, std::back_inserter(allocation_extents));
        auto source_mismatches = allocation_mismatch_rows(source.path, container.partitions());
        std::ranges::move(source_mismatches, std::back_inserter(allocation_mismatches));
        const auto prior_issue_count = issues.size();
        auto source_volumes = volume_validation_rows(source.path, container, source.catalog, volume_issues, issues);
        for (auto index = prior_issue_count; index < issues.size(); ++index) {
            const auto code =
                std::ranges::find(issues[index], "code", &std::pair<std::string, axk::ReportValue>::first);
            if (code != issues[index].end())
                ++issue_counts[std::get<std::string>(code->second.value)];
            if (request->policy == "strict")
                failed = true;
        }
        std::ranges::move(source_volumes, std::back_inserter(volumes));
    }

    axk::ReportValue::Object summary_counts;
    for (const auto &[name, count] : issue_counts)
        summary_counts.emplace_back(name, count);
    axk::ReportRow validation_summary{{"policy", request->policy},
                                      {"failed", failed},
                                      {"issue_count", static_cast<std::uint64_t>(issues.size())},
                                      {"summary_counts", std::move(summary_counts)}};
    std::uint64_t pass_count{};
    std::uint64_t warn_count{};
    std::uint64_t fail_count{};
    std::uint64_t fatal_issue_count{};
    std::uint64_t warning_issue_count{};
    std::uint64_t malformed_category_entry_count{};
    std::uint64_t allocation_issue_count{};
    for (const auto &row : volumes) {
        const auto text = [&](std::string_view key) -> std::string {
            const auto found = std::ranges::find(row, key, &std::pair<std::string, axk::ReportValue>::first);
            return found == row.end() ? std::string{} : std::get<std::string>(found->second.value);
        };
        const auto number = [&](std::string_view key) -> std::uint64_t {
            const auto found = std::ranges::find(row, key, &std::pair<std::string, axk::ReportValue>::first);
            return found == row.end() ? 0U : std::get<std::uint64_t>(found->second.value);
        };
        if (text("validation_status") == "Pass")
            ++pass_count;
        else if (text("validation_status") == "Warn")
            ++warn_count;
        else
            ++fail_count;
        fatal_issue_count += number("fatal_issue_count");
        warning_issue_count += number("warning_issue_count");
        malformed_category_entry_count += number("malformed_category_entry_count");
        allocation_issue_count += number("allocation_issue_count");
    }
    axk::ReportRow volume_summary{
        {"source_image", loaded.size() == 1U ? axk::text::path_to_utf8(loaded.front().path) : ""},
        {"volume_count", static_cast<std::uint64_t>(volumes.size())},
        {"pass_count", pass_count},
        {"warn_count", warn_count},
        {"fail_count", fail_count},
        {"fatal_issue_count", fatal_issue_count},
        {"warning_issue_count", warning_issue_count},
        {"malformed_category_entry_count", malformed_category_entry_count},
        {"allocation_issue_count", allocation_issue_count},
    };

    std::vector<axk::ReportSchemaManifest> schemas;
    auto validation_issue_schema =
        write_report_set(*destination, request->destination, "validation_issues", issues, request->overwrite);
    if (!validation_issue_schema)
        return std::unexpected(validation_issue_schema.error());
    schemas.push_back(std::move(*validation_issue_schema));
    if (auto written =
            axk::write_report_object(*destination / "validation_summary.json", validation_summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "validation_summary.json")}));
    }
    axk::ReportSchemaOptions schema_options;
    schema_options.source_command = "axklib";
    schema_options.library_version = std::string{axk::version()};
    auto validation_summary_schema =
        axk::make_report_schema("validation_summary", std::span{&validation_summary, 1U}, schema_options);
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "validation_summary.schema.json",
                                                validation_summary_schema, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id,
                              child_reference_path(request->destination, "_schemas/validation_summary.schema.json")}));
    }
    schemas.push_back(validation_summary_schema);
    if (!request->exports && has_sfs_input) {
        for (const auto &[name, rows] :
             std::initializer_list<std::pair<std::string_view, const std::vector<axk::ReportRow> &>>{
                 {"allocation_summary", allocation_summaries},
                 {"allocation_extents", allocation_extents},
                 {"allocation_mismatches", allocation_mismatches},
                 {"volume_validation", volumes},
                 {"volume_validation_issues", volume_issues}}) {
            auto schema =
                write_report_set(*destination, request->destination, std::string{name}, rows, request->overwrite);
            if (!schema)
                return std::unexpected(schema.error());
            schemas.push_back(std::move(*schema));
        }
        const std::vector<axk::ReportRow> volume_summaries{volume_summary};
        auto volume_schema = write_report_set(*destination, request->destination, "volume_validation_summary",
                                              volume_summaries, request->overwrite);
        if (!volume_schema)
            return std::unexpected(volume_schema.error());
        schemas.push_back(std::move(*volume_schema));
    }
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }

    auto artifacts = Json::array();
    for (const auto &schema : schemas) {
        if (schema.report_name == "validation_summary") {
            artifacts.push_back(
                {{"rootId", request->destination.root_id},
                 {"relativePath", child_reference_path(request->destination, "validation_summary.json")}});
        } else {
            artifacts.push_back(
                {{"rootId", request->destination.root_id},
                 {"relativePath", child_reference_path(request->destination, schema.report_name + ".csv")}});
            artifacts.push_back(
                {{"rootId", request->destination.root_id},
                 {"relativePath", child_reference_path(request->destination, schema.report_name + ".json")}});
        }
        artifacts.push_back(
            {{"rootId", request->destination.root_id},
             {"relativePath",
              child_reference_path(request->destination, std::format("_schemas/{}.schema.json", schema.report_name))}});
    }
    artifacts.push_back({{"rootId", request->destination.root_id},
                         {"relativePath", child_reference_path(request->destination, "_schemas/schema_index.json")}});
    if (context.progress != nullptr) {
        context.progress->report({axk::ProgressPhase::writing, request->sources.size(), request->sources.size(),
                                  "validation", std::nullopt});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    return Json{{"operationId", "report.validate"}, {"sourceCount", request->sources.size()},
                {"issueCount", issues.size()},      {"failed", failed},
                {"policy", request->policy},        {"artifacts", std::move(artifacts)}};
}

} // namespace axk::app::validation_operations_internal

axk::app::Result<void> axk::app::bind_validation_operations(OperationRegistry &registry, const Sandbox &sandbox) {
    if (registry.is_implemented("report.validate"))
        return {};
    return registry.bind(
        "report.validate",
        [&sandbox](const nlohmann::json &request, const OperationContext &context) -> Result<nlohmann::json> {
            return validation_operations_internal::execute_validation(sandbox, request, context);
        });
}
