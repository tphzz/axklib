#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "handlers.hpp"
#include "local_operations.hpp"
#include "requests.hpp"
#include "support.hpp"

#include "axklib/utf8.hpp"

namespace axk::cli::commands {

using Json = nlohmann::json;

int run_objects_request(const axk::cli::ObjectsRequest &request) {
    if (!request.output_directory)
        return 2;
    const auto paths = expand_cli_paths(request.paths);
    auto runtime_paths = paths;
    runtime_paths.push_back(*request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = Json::array();
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back({{"rootId", reference->root_id}, {"relativePath", reference->relative_path}});
    }
    auto destination = (*runtime)->directory_ref(*request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    Json input{{"sources", std::move(sources)},
               {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
               {"overwrite", request.overwrite},
               {"strict", request.strict},
               {"includePayloads", request.with_payloads},
               {"pretty", request.pretty}};
    if (request.object_type)
        input["objectType"] = *request.object_type;
    auto result = (*runtime)->invoke("report.objects", input);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "objects=" << result->at("rowCount").get<std::size_t>()
              << " load_errors=" << result->at("failedCount").get<std::size_t>() << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(*request.output_directory) << '\n';
    return result->at("failedCount").get<std::size_t>() == 0U ? 0 : 3;
}

int run_inventory_request(const axk::cli::InventoryRequest &request) {
    const auto paths = expand_cli_paths(request.paths);
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = Json::array();
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back({{"rootId", reference->root_id}, {"relativePath", reference->relative_path}});
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    auto result = (*runtime)->invoke(
        "report.inventory",
        {{"sources", std::move(sources)},
         {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
         {"overwrite", request.overwrite},
         {"strict", request.strict}});
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "objects=" << result->at("rowCount").get<std::size_t>()
              << " decode_issues=" << result->at("decodeIssueCount").get<std::size_t>()
              << " load_errors=" << result->at("failedCount").get<std::size_t>() << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    return result->at("failedCount").get<std::size_t>() == 0U ? 0 : 1;
}

int run_orphans_request(const axk::cli::OrphansRequest &request) {
    const auto paths = expand_cli_paths(request.paths);
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = Json::array();
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back({{"rootId", reference->root_id}, {"relativePath", reference->relative_path}});
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    auto result = (*runtime)->invoke(
        "report.orphans",
        {{"sources", std::move(sources)},
         {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
         {"overwrite", request.overwrite}});
    if (!result)
        return axk::cli::report_application_failure(result.error());
    for (const auto &summary : result->at("summaries")) {
        std::cout << "image=" << summary.at("sourcePath").get<std::string>()
                  << " waveforms=" << summary.at("waveformCount").get<std::size_t>()
                  << " referenced=" << summary.at("referencedCount").get<std::size_t>()
                  << " known_unreferenced=" << summary.at("knownUnreferencedCount").get<std::size_t>()
                  << " ambiguous_or_unresolved=" << summary.at("ambiguousOrUnresolvedCount").get<std::size_t>() << '\n';
    }
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    return 0;
}

int run_validate_request(const axk::cli::ValidateRequest &request) {
    if (!request.exports && request.paths.empty()) {
        std::cerr << "validate requires input paths unless --exports is supplied\n";
        return 2;
    }
    const auto paths = request.exports ? std::vector<std::filesystem::path>{} : expand_cli_paths(request.paths);
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    if (request.exports)
        runtime_paths.push_back(*request.exports);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = Json::array();
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back({{"rootId", reference->root_id}, {"relativePath", reference->relative_path}});
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    Json input{{"sources", std::move(sources)},
               {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
               {"policy", request.policy},
               {"overwrite", request.overwrite}};
    if (request.exports) {
        auto exports = (*runtime)->directory_ref(*request.exports);
        if (!exports)
            return axk::cli::report_application_failure(exports.error());
        input["exports"] = {{"rootId", exports->root_id}, {"relativePath", exports->relative_path}};
    }
    auto result = (*runtime)->invoke("report.validate", input);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    const auto failed = result->at("failed").get<bool>();
    std::cout << "issues=" << result->at("issueCount").get<std::size_t>() << " failed=" << (failed ? "True" : "False")
              << " policy=" << result->at("policy").get<std::string>() << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    return failed ? 1 : 0;
}

int run_corpus_audit_request(const axk::cli::CorpusAuditRequest &request) {
    const auto paths = expand_cli_paths(request.paths);
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = Json::array();
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back({{"rootId", reference->root_id}, {"relativePath", reference->relative_path}});
    }
    auto destination = (*runtime)->directory_ref(request.output_directory);
    if (!destination)
        return axk::cli::report_application_failure(destination.error());
    auto result = (*runtime)->invoke(
        "corpus.audit",
        {{"sources", std::move(sources)},
         {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
         {"policy", request.policy},
         {"waveSmokeLimit", request.wave_smoke_limit},
         {"skipWaveSmoke", request.skip_wave_smoke},
         {"overwrite", request.overwrite}});
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "containers=" << result->at("loadedCount").get<std::size_t>()
              << " objects=" << result->at("objectCount").get<std::size_t>()
              << " validation_issues=" << result->at("validationIssueCount").get<std::size_t>()
              << " relationships=" << result->at("relationshipCount").get<std::size_t>()
              << " wave_smoke=" << result->at("waveSmokeDecoded").get<std::size_t>() << '/'
              << result->at("waveSmokeErrorCount").get<std::size_t>() << '\n';
    std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
    if (result->at("failedCount").get<std::size_t>() != 0U)
        return 3;
    return result->at("validationFailed").get<bool>() ? 1 : 0;
}

} // namespace axk::cli::commands
