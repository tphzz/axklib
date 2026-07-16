#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

#include "content_id.hpp"
#include "handlers.hpp"
#include "local_operations.hpp"
#include "requests.hpp"
#include "schema/info_v1.hpp"
#include "schema/operations_v1.hpp"
#include "support.hpp"

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/error.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/terminology.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"

namespace axk::cli::commands {

int run_relationships_request(const axk::cli::RelationshipsRequest &request) {
    const auto paths = expand_cli_paths(request.paths);
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = nlohmann::json::array();
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
        "report.relationships",
        {{"sources", std::move(sources)},
         {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
         {"overwrite", request.overwrite}});
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "relationships=" << result->at("rowCount").get<std::size_t>()
              << " ambiguous=" << result->at("ambiguousCount").get<std::size_t>()
              << " load_errors=" << result->at("failedCount").get<std::size_t>() << '\n';
    return result->at("failedCount").get<std::size_t>() == 0U ? 0 : 3;
}

int run_coverage_request(const axk::cli::CoverageRequest &request) {
    const auto paths = expand_cli_paths(request.paths);
    auto runtime_paths = paths;
    runtime_paths.push_back(request.output_directory);
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = nlohmann::json::array();
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
        "report.coverage",
        {{"sources", std::move(sources)},
         {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
         {"overwrite", request.overwrite}});
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "relationships=" << result->at("rowCount").get<std::size_t>()
              << " load_errors=" << result->at("failedCount").get<std::size_t>() << '\n';
    return result->at("failedCount").get<std::size_t>() == 0U ? 0 : 3;
}

axk::cli::schema::info_v1::NodeOutput info_node_output(const nlohmann::json &node) {
    axk::cli::schema::info_v1::NodeOutput result{
        .node_id = node.at("nodeId").get<std::string>(),
        .node_type = node.at("nodeType").get<std::string>(),
        .display_name = node.at("displayName").get<std::string>(),
        .object_key = node.at("objectKey").get<std::string>(),
        .object_type = node.at("objectType").get<std::string>(),
        .count = node.at("count").is_null() ? std::nullopt
                                            : std::optional<std::uint64_t>{node.at("count").get<std::uint64_t>()},
        .details = node.at("details").get<std::vector<std::string>>(),
        .quality = node.at("quality").get<std::string>(),
        .basis = node.at("basis").get<std::string>(),
        .notes = node.at("notes").get<std::string>(),
        .selector_path = node.at("selectorPath").get<std::string>(),
        .children = {},
    };
    for (const auto &child : node.at("children"))
        result.children.push_back(info_node_output(child));
    return result;
}

axk::cli::schema::info_v1::InfoOutput info_output(const nlohmann::json &service_result) {
    axk::cli::schema::info_v1::InfoOutput result;
    for (const auto &tree : service_result.at("trees")) {
        axk::cli::schema::info_v1::TreeOutput projected{
            .source_path_utf8 = tree.at("sourcePath").get<std::string>(),
            .container_kind = tree.at("containerKind").get<std::string>(),
            .detected_format = tree.at("detectedFormat").get<std::string>(),
            .object_count = tree.at("objectCount").get<std::uint64_t>(),
            .object_counts = tree.at("objectCounts").get<std::map<std::string, std::uint64_t>>(),
            .recovery = tree.at("recovery").is_null()
                            ? std::nullopt
                            : std::optional<std::string>{tree.at("recovery").get<std::string>()},
            .roots = {},
            .issues = {},
        };
        for (const auto &root : tree.at("roots"))
            projected.roots.push_back(info_node_output(root));
        for (const auto &issue : tree.at("issues")) {
            projected.issues.push_back({.code = issue.at("code").get<std::string>(),
                                        .severity = issue.at("severity").get<std::string>(),
                                        .message = issue.at("message").get<std::string>(),
                                        .source_path_utf8 = issue.at("sourcePath").get<std::string>(),
                                        .sampler_path = issue.at("samplerPath").get<std::string>(),
                                        .object_key = issue.at("objectKey").get<std::string>()});
        }
        result.trees.push_back(std::move(projected));
    }
    for (const auto &error : service_result.at("loadErrors")) {
        result.load_errors.push_back({.path_utf8 = error.at("path").get<std::string>(),
                                      .error_code = error.at("errorCode").get<std::uint64_t>(),
                                      .message = error.at("message").get<std::string>(),
                                      .original_exception = error.at("originalException").get<std::string>()});
    }
    return result;
}

std::string tree_type_label(const axk::cli::schema::info_v1::NodeOutput &node) {
    if (node.node_type == "partition")
        return "PARTITION";
    if (node.node_type == "volume")
        return "VOLUME";
    if (node.node_type == "category")
        return "CATEGORY";
    if (node.node_type == "recovery_artifact")
        return "RECOVERY";
    if (node.node_type == "unresolved" || node.node_type == "relationship_target" ||
        node.node_type == "unresolved_program_assignment")
        return "UNKNOWN";
    return std::string{axk::sampler_object_tree_label(node.object_type)};
}

void render_tree_text(const axk::cli::schema::info_v1::NodeOutput &node, std::size_t depth, std::string prefix,
                      bool last, const axk::cli::InfoRequest &request) {
    if (request.max_depth && depth > *request.max_depth)
        return;
    std::cout << prefix << (last ? "`-- " : "|-- ") << node.display_name;
    const auto label = tree_type_label(node);
    if (!label.empty())
        std::cout << " [" << label << ']';
    if (node.count)
        std::cout << " (" << *node.count << ')';
    if (!node.details.empty()) {
        std::cout << " - ";
        for (std::size_t index = 0; index < node.details.size(); ++index) {
            if (index != 0U)
                std::cout << "; ";
            std::cout << node.details[index];
        }
    }
    if (request.show_quality || node.quality != "Known")
        std::cout << " [" << node.quality << ']';
    if (!node.notes.empty() && (node.quality == "Unknown" || node.quality == "Tentative"))
        std::cout << " - " << node.notes;
    std::cout << '\n';
    const auto child_prefix = prefix + (last ? "    " : "|   ");
    for (std::size_t index = 0; index < node.children.size(); ++index)
        render_tree_text(node.children[index], depth + 1U, child_prefix, index + 1U == node.children.size(), request);
}

void render_tree_paths(const axk::cli::schema::info_v1::TreeOutput &tree,
                       const axk::cli::schema::info_v1::NodeOutput &node) {
    std::string scope;
    if (node.node_type == "volume")
        scope = "volume";
    else if (node.object_type == "PROG")
        scope = "program";
    else if (node.object_type == "SBAC")
        scope = "sbac";
    else if (node.object_type == "SBNK")
        scope = "sbnk";
    if (!scope.empty()) {
        std::cout << tree.source_path_utf8 << '\t' << scope << '\t' << node.selector_path << '\t' << node.display_name
                  << '\t' << node.object_type << '\t' << node.object_key << '\n';
    }
    for (const auto &child : node.children)
        render_tree_paths(tree, child);
}

int run_info_request(const axk::cli::InfoRequest &request) {
    const auto paths = expand_cli_paths(request.paths);
    if (paths.empty()) {
        if (request.format == "json")
            std::cout << "{\n  \"trees\": [],\n  \"load_errors\": []\n}\n";
        return 0;
    }
    auto runtime = axk::cli::LocalOperationRuntime::create(paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    auto sources = nlohmann::json::array();
    for (const auto &path : paths) {
        auto reference = (*runtime)->file_ref(path);
        if (!reference)
            return axk::cli::report_application_failure(reference.error());
        sources.push_back({{"rootId", reference->root_id}, {"relativePath", reference->relative_path}});
    }
    auto service_result =
        (*runtime)->invoke("report.info", {{"sources", std::move(sources)},
                                           {"strict", request.strict},
                                           {"includeDefaultPrograms", request.show_default_programs}});
    if (!service_result)
        return axk::cli::report_application_failure(service_result.error());
    const auto output = info_output(*service_result);
    if (request.format == "json") {
        const auto serialized = axk::cli::schema::info_v1::serialize(output);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
        return output.load_errors.empty() ? 0 : 1;
    }
    for (const auto &error : output.load_errors) {
        std::cout << error.path_utf8 << "\tERROR\tAXKLIB_CONTAINER_OPEN_FAILED\t" << error.message << '\n';
    }
    if (request.format == "summary") {
        for (const auto &tree : output.trees) {
            std::cout << tree.source_path_utf8 << '\t' << tree.container_kind << "\tobjects=" << tree.object_count;
            for (const auto &[type, count] : tree.object_counts)
                std::cout << ' ' << type << '=' << count;
            std::cout << "\trecovery=" << tree.recovery.value_or("-") << '\n';
        }
        return output.load_errors.empty() ? 0 : 1;
    }
    for (const auto &tree : output.trees) {
        if (request.format == "paths")
            std::cout << "source_path\tscope\tpath\tdisplay_name\tobject_"
                         "type\tobject_key\n";
        else
            std::cout << tree.source_path_utf8 << " [" << tree.container_kind << "]\n";
        for (std::size_t index = 0; index < tree.roots.size(); ++index) {
            const auto &root = tree.roots[index];
            if (request.format == "paths")
                render_tree_paths(tree, root);
            else
                render_tree_text(root, 1U, {}, index + 1U == tree.roots.size(), request);
        }
    }
    return output.load_errors.empty() ? 0 : 1;
}

} // namespace axk::cli::commands
