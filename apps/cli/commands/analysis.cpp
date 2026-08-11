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
    auto result = (*runtime)->report("report.relationships", sources, *destination, request.overwrite);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "relationships=" << result->row_count << " ambiguous=" << result->ambiguous_count
              << " load_errors=" << result->failed_count << '\n';
    return exit_code(result->failed_count == 0U ? ExitStatus::success : ExitStatus::diagnostics);
}

int run_coverage_request(const axk::cli::CoverageRequest &request) {
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
    auto result = (*runtime)->report("report.coverage", sources, *destination, request.overwrite);
    if (!result)
        return axk::cli::report_application_failure(result.error());
    std::cout << "relationships=" << result->row_count << " load_errors=" << result->failed_count << '\n';
    return exit_code(result->failed_count == 0U ? ExitStatus::success : ExitStatus::diagnostics);
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
    auto expanded = expand_cli_paths(request.paths, true);
    if (!expanded)
        return report_failure(expanded.error());
    const auto &paths = *expanded;
    if (paths.empty()) {
        if (request.format == "json")
            std::cout << "{\n  \"trees\": [],\n  \"load_errors\": []\n}\n";
        return exit_code(ExitStatus::success);
    }
    auto runtime = axk::cli::LocalOperationRuntime::create(paths);
    if (!runtime)
        return axk::cli::report_application_failure(runtime.error());
    std::vector<LocalInfoSource> sources;
    for (const auto &path : paths) {
        std::error_code status_error;
        const auto directory = std::filesystem::is_directory(path, status_error);
        if (status_error)
            return report_failure(
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not inspect info source path"));
        if (directory) {
            auto reference = (*runtime)->directory_ref(path);
            if (!reference)
                return axk::cli::report_application_failure(reference.error());
            sources.push_back({.file = std::nullopt, .object_directory = std::move(*reference)});
        } else {
            auto reference = (*runtime)->file_ref(path);
            if (!reference)
                return axk::cli::report_application_failure(reference.error());
            sources.push_back({.file = std::move(*reference), .object_directory = std::nullopt});
        }
    }
    auto output_result = (*runtime)->info(sources, request.strict, request.show_default_programs);
    if (!output_result)
        return axk::cli::report_application_failure(output_result.error());
    const auto &output = *output_result;
    if (request.format == "json") {
        const auto serialized = axk::cli::schema::info_v1::serialize(output);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
        return exit_code(output.load_errors.empty() ? ExitStatus::success : ExitStatus::diagnostics);
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
        return exit_code(output.load_errors.empty() ? ExitStatus::success : ExitStatus::diagnostics);
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
    return exit_code(output.load_errors.empty() ? ExitStatus::success : ExitStatus::diagnostics);
}

} // namespace axk::cli::commands
