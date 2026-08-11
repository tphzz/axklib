#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "handlers.hpp"
#include "local_operations.hpp"
#include "requests.hpp"
#include "support.hpp"

#include "axklib/utf8.hpp"

namespace axk::cli::commands {

int run_objects_request(const axk::cli::ObjectsRequest &request) {
    if (!request.output_directory)
        return exit_code(ExitStatus::invalid_request);
    auto expanded = expand_cli_paths(request.paths);
    if (!expanded)
        return report_failure(expanded.error());
    const auto &paths = *expanded;
    auto runtime_paths = paths;
    runtime_paths.push_back(*request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    std::vector<app::FileRef> sources;
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back(std::move(*reference));
    }
    auto destination = (*runtime)->directory_ref(*request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    auto result = (*runtime)->report("report.objects", sources, *destination, request.overwrite, request.strict,
                                     request.with_payloads, request.pretty, request.object_type);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "objects=" << result->row_count << " load_errors=" << result->failed_count << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(*request.output_directory) << '\n';
    return exit_code(result->failed_count == 0U ? ExitStatus::success : ExitStatus::diagnostics);
}

int run_inventory_request(const axk::cli::InventoryRequest &request) {
    auto expanded = expand_cli_paths(request.paths);
    if (!expanded)
        return report_failure(expanded.error());
    const auto &paths = *expanded;
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    std::vector<app::FileRef> sources;
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back(std::move(*reference));
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    auto result = (*runtime)->report("report.inventory", sources, *destination, request.overwrite, request.strict);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "objects=" << result->row_count << " decode_issues=" << result->decode_issue_count
              << " load_errors=" << result->failed_count << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    return exit_code(result->failed_count == 0U ? ExitStatus::success : ExitStatus::diagnostics);
}

int run_orphans_request(const axk::cli::OrphansRequest &request) {
    auto expanded = expand_cli_paths(request.paths);
    if (!expanded)
        return report_failure(expanded.error());
    const auto &paths = *expanded;
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    std::vector<app::FileRef> sources;
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back(std::move(*reference));
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    auto result = (*runtime)->report_orphans(sources, *destination, request.overwrite);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    for (const auto &summary : *result) {
        std::cout << "image=" << summary.source_path << " wave_data=" << summary.waveform_count
                  << " referenced=" << summary.referenced_count
                  << " known_unreferenced=" << summary.known_unreferenced_count
                  << " ambiguous_or_unresolved=" << summary.ambiguous_or_unresolved_count << '\n';
    }
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    return exit_code(ExitStatus::success);
}

int run_validate_request(const axk::cli::ValidateRequest &request) {
    if (!request.exports && request.paths.empty()) {
        std::cerr << "validate requires input paths unless --exports is supplied\n";
        return exit_code(ExitStatus::invalid_request);
    }
    auto expanded = request.exports ? Result<std::vector<std::filesystem::path>>{std::vector<std::filesystem::path>{}}
                                    : expand_cli_paths(request.paths);
    if (!expanded)
        return report_failure(expanded.error());
    const auto &paths = *expanded;
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    if (request.exports)
        runtime_paths.push_back(*request.exports);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    std::vector<app::FileRef> sources;
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back(std::move(*reference));
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    std::optional<app::DirectoryRef> exports;
    if (request.exports) {
        auto reference = (*runtime)->directory_ref(*request.exports);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        exports = std::move(*reference);
    }
    auto result = (*runtime)->report_validation(sources, *destination, exports, request.policy, request.overwrite);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "issues=" << result->issue_count << " failed=" << (result->failed ? "True" : "False")
              << " policy=" << result->policy << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    return exit_code(result->failed ? ExitStatus::diagnostics : ExitStatus::success);
}

int run_corpus_audit_request(const axk::cli::CorpusAuditRequest &request) {
    auto expanded = expand_cli_paths(request.paths);
    if (!expanded)
        return report_failure(expanded.error());
    const auto &paths = *expanded;
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    std::vector<app::FileRef> sources;
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back(std::move(*reference));
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    auto result = (*runtime)->corpus_audit(sources, *destination, request.policy, request.wave_smoke_limit,
                                           request.skip_wave_smoke, request.overwrite);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "containers=" << result->loaded_count << " objects=" << result->object_count
              << " validation_issues=" << result->validation_issue_count
              << " relationships=" << result->relationship_count << " wave_smoke=" << result->wave_smoke_decoded << '/'
              << result->wave_smoke_error_count << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    if (result->failed_count != 0U)
        return exit_code(ExitStatus::diagnostics);
    return exit_code(result->validation_failed ? ExitStatus::diagnostics : ExitStatus::success);
}

} // namespace axk::cli::commands
