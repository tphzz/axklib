#include <filesystem>
#include <iostream>
#include <string>

#include "../exit_status.hpp"
#include "handlers.hpp"
#include "local_operations.hpp"
#include "requests.hpp"
#include "support.hpp"

namespace axk::cli::commands {

int run_extract_request(const axk::cli::ExtractRequest &request) {
    if (request.scope != "file" && request.selector_paths.empty()) {
        std::cerr << "extract " << request.scope
                  << " requires at least one --path from `axklib info --format "
                     "paths`\n";
        return axk::cli::exit_code(axk::cli::ExitStatus::invalid_request);
    }
    auto expanded = expand_cli_paths(request.paths);
    if (!expanded)
        return report_failure(expanded.error());
    auto sources = std::move(*expanded);
    std::vector<std::filesystem::path> runtime_paths = sources;
    runtime_paths.push_back(request.output_directory);
    auto runtime = LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return report_application_failure(runtime.error());

    std::vector<app::FileRef> source_refs;
    for (const auto &source : sources) {
        auto reference = (*runtime)->file_ref(source);
        if (!reference)
            return report_application_failure(reference.error());
        source_refs.push_back(std::move(*reference));
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return report_application_failure(destination.error());
    auto result = (*runtime)->extract(request.sfz, source_refs, *destination, request.scope, request.selector_paths,
                                      request.stereo, request.overwrite, request.strict);
    if (!result) {
        if (result.error().code == "selector_not_found" || result.error().code == "unsupported_selection_scope") {
            std::cerr << result.error().message << ". Run `axklib info --format paths` and copy the path column.\n";
            return axk::cli::exit_code(axk::cli::ExitStatus::invalid_request);
        }
        std::cerr << "error: " << result.error().message << '\n';
        return axk::cli::exit_code(axk::cli::ExitStatus::operational_failure);
    }
    for (const auto &warning : result->warnings) {
        if (warning.code == "waveform_skipped")
            std::cerr << "warning: skipped Wave Data " << warning.message << '\n';
    }
    std::cout << "wave_data=" << result->waveform_count << " written_files=" << result->written_file_count
              << " selection_graphs=" << result->selection_graph_count << " sfz_files=" << result->sfz_file_count
              << " decode_errors=" << result->decode_error_count << " load_errors=" << result->load_error_count << '\n';
    return axk::cli::exit_code(result->load_error_count == 0U ? axk::cli::ExitStatus::success
                                                              : axk::cli::ExitStatus::operational_failure);
}

} // namespace axk::cli::commands
