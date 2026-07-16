#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

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
        return 4;
    }
    auto sources = expand_cli_paths(request.paths);
    std::vector<std::filesystem::path> runtime_paths = sources;
    runtime_paths.push_back(request.output_directory);
    auto runtime = LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return report_application_failure(runtime.error());

    auto source_refs = nlohmann::json::array();
    for (const auto &source : sources) {
        auto reference = (*runtime)->file_ref(source);
        if (!reference)
            return report_application_failure(reference.error());
        source_refs.push_back({{"rootId", reference->root_id}, {"relativePath", reference->relative_path}});
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return report_application_failure(destination.error());
    nlohmann::json input{
        {"sources", std::move(source_refs)},
        {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
        {"scope", request.scope},
        {"selectors", request.selector_paths},
        {"stereo", request.stereo},
        {"overwrite", request.overwrite},
        {"strict", request.strict}};
    auto result = (*runtime)->invoke(request.sfz ? "extract.sfz" : "extract.wav", input);
    if (!result) {
        if (result.error().code == "selector_not_found" || result.error().code == "unsupported_selection_scope") {
            std::cerr << result.error().message << ". Run `axklib info --format paths` and copy the path column.\n";
            return 4;
        }
        std::cerr << "error: " << result.error().message << '\n';
        return 1;
    }
    for (const auto &warning : result->at("warnings")) {
        if (warning.at("code") == "waveform_skipped")
            std::cerr << "warning: skipped Wave Data " << warning.at("message").get<std::string>() << '\n';
    }
    std::cout << "wave_data=" << result->at("waveformCount").get<std::size_t>()
              << " written_files=" << result->at("writtenFileCount").get<std::size_t>()
              << " selection_graphs=" << result->at("selectionGraphCount").get<std::size_t>()
              << " sfz_files=" << result->at("sfzFileCount").get<std::size_t>()
              << " decode_errors=" << result->at("decodeErrorCount").get<std::size_t>()
              << " load_errors=" << result->at("loadErrorCount").get<std::size_t>() << '\n';
    return result->at("loadErrorCount").get<std::size_t>() == 0U ? 0 : 1;
}

} // namespace axk::cli::commands
