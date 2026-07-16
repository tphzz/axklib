#include "handlers.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "local_operations.hpp"
#include "package_projection.hpp"
#include "requests.hpp"
#include "schema/package_v1.hpp"
#include "support.hpp"

#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/utf8.hpp"

namespace axk::cli::commands {
namespace {

using Json = nlohmann::json;

Json file_ref_json(const app::FileRef &reference) {
    return {{"rootId", reference.root_id}, {"relativePath", reference.relative_path}};
}

Error argument_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::internal, std::move(message));
}

Result<PackageRootKind> parse_root_kind(std::string_view value) {
    if (value == "volume")
        return PackageRootKind::volume;
    if (value == "program" || value == "prog")
        return PackageRootKind::prog;
    if (value == "sbac")
        return PackageRootKind::sbac;
    if (value == "sbnk")
        return PackageRootKind::sbnk;
    if (value == "smpl" || value == "sample")
        return PackageRootKind::smpl;
    return std::unexpected{argument_error("package root kind must be volume, program, sbac, sbnk, "
                                          "or smpl")};
}

Result<PackageRootSelector> parse_root(const std::string &value, const axk::cli::PackageExportRequest &request) {
    const auto separator = value.find('=');
    const auto kind_text = value.substr(0U, separator);
    auto kind = parse_root_kind(kind_text);
    if (!kind)
        return std::unexpected{kind.error()};
    const auto object_name = separator == std::string::npos ? std::string{} : value.substr(separator + 1U);
    if (*kind == PackageRootKind::volume) {
        if (separator != std::string::npos)
            return std::unexpected{argument_error("volume package roots do not take an object name")};
    } else if (separator == std::string::npos || object_name.empty()) {
        return std::unexpected{argument_error("object package roots use KIND=NAME, for example program=001")};
    }
    if (request.partition_index && *request.partition_index > std::numeric_limits<std::uint8_t>::max()) {
        return std::unexpected{argument_error("package source partition index is out of range")};
    }
    PackageRootSelector result;
    result.kind = *kind;
    if (request.partition_index)
        result.partition_index = static_cast<std::uint8_t>(*request.partition_index);
    result.group_name = request.group_name;
    result.volume_name = request.volume_name;
    result.object_name = object_name;
    return result;
}

bool has_only_fields(const Json &object, const std::set<std::string, std::less<>> &fields) {
    if (!object.is_object())
        return false;
    for (const auto &[key, value] : object.items()) {
        static_cast<void>(value);
        if (!fields.contains(key))
            return false;
    }
    return true;
}

Result<PackageRootDestination> parse_destination(const std::string &value) {
    try {
        const auto object = Json::parse(value);
        static const std::set<std::string, std::less<>> fields{"package", "root",      "partition",  "group",
                                                               "volume",  "raw_group", "raw_volume", "create"};
        if (!has_only_fields(object, fields) || !object.contains("package") || !object.contains("root")) {
            return std::unexpected{argument_error("package destination must be a JSON object with "
                                                  "package and root indexes")};
        }
        PackageRootDestination result;
        result.package_index = object.at("package").get<std::size_t>();
        result.root_index = object.at("root").get<std::size_t>();
        if (object.contains("partition")) {
            const auto partition = object.at("partition").get<std::uint32_t>();
            if (partition > std::numeric_limits<std::uint8_t>::max())
                return std::unexpected{argument_error("package destination partition is out of range")};
            result.partition_index = static_cast<std::uint8_t>(partition);
        }
        result.group_name = object.value("group", std::string{});
        result.volume_name = object.value("volume", std::string{});
        result.raw_group = object.value("raw_group", std::string{});
        result.raw_volume = object.value("raw_volume", std::string{});
        result.create_destination = object.value("create", false);
        return result;
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{argument_error(std::string{"invalid package destination JSON: "} + error.what())};
    }
}

Result<std::vector<PackageNodeRename>> load_renames(const std::filesystem::path &path) {
    std::ifstream input{path};
    if (!input)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open package rename map")};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input && !input.eof())
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not read package rename map")};
    const auto text_value = buffer.str();
    if (!text::is_valid_utf8(text_value))
        return std::unexpected{argument_error("package rename map is not valid UTF-8")};
    try {
        const auto array = Json::parse(text_value);
        if (!array.is_array())
            return std::unexpected{argument_error("package rename map must be a JSON array")};
        std::vector<PackageNodeRename> result;
        result.reserve(array.size());
        static const std::set<std::string, std::less<>> fields{"package", "node_id", "name"};
        for (const auto &item : array) {
            if (!has_only_fields(item, fields) || !item.contains("package") || !item.contains("node_id") ||
                !item.contains("name")) {
                return std::unexpected{argument_error("each package rename must contain only "
                                                      "package, node_id, and name")};
            }
            result.push_back({item.at("package").get<std::size_t>(), item.at("node_id").get<std::string>(),
                              item.at("name").get<std::string>()});
        }
        return result;
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{argument_error(std::string{"invalid package rename map JSON: "} + error.what())};
    }
}

void print_package_summary(const schema::package_v1::PackageOutput &output, bool verify_only) {
    std::cout << output.path_utf8 << '\t' << (output.valid ? "valid" : "invalid") << "\tkind=" << output.package_kind
              << "\tpackage_id=" << output.package_id << "\troots=" << output.roots.size()
              << "\tobjects=" << output.objects.size() << "\trelationships=" << output.relationship_count;
    std::cout << "\tverification=" << (output.payloads_verified ? "full" : "manifest");
    if (!verify_only)
        std::cout << "\textension=" << output.required_extension;
    std::cout << '\n';
    for (const auto &issue : output.issues)
        std::cout << (issue.fatal ? "error" : "warning") << '\t' << issue.code << '\t' << issue.message << '\n';
}

void print_plan_summary(const schema::package_v1::PlanOutput &output) {
    std::cout << output.target_path_utf8 << '\t' << (output.valid ? "valid" : "conflicts")
              << "\ttarget=" << output.target_kind << "\tplan_id=" << output.plan_id
              << "\tobjects=" << output.objects.size() << "\tconflicts=" << output.conflicts.size() << '\n';
    for (const auto &warning : output.warnings)
        std::cout << "warning\t" << warning.code << '\t' << warning.message << '\n';
    for (const auto &conflict : output.conflicts)
        std::cout << "conflict\t" << conflict.code << '\t' << conflict.message << '\n';
    if (output.result)
        std::cout << output.result->output_path_utf8
                  << "\tapplied\toutput_snapshot_id=" << output.result->output_snapshot_id << '\n';
}

} // namespace

int run_package_export(const axk::cli::PackageExportRequest &request) {
    std::vector<PackageRootSelector> roots;
    roots.reserve(request.roots.size());
    for (const auto &value : request.roots) {
        auto root = parse_root(value, request);
        if (!root)
            return report_failure(root.error());
        roots.push_back(std::move(*root));
    }

    const std::array paths{request.source, request.output};
    auto runtime = LocalOperationRuntime::create(paths);
    if (!runtime)
        return report_application_failure(runtime.error());
    auto source_ref = (*runtime)->file_ref(request.source);
    auto output_ref = (*runtime)->file_ref(request.output);
    if (!source_ref)
        return report_application_failure(source_ref.error());
    if (!output_ref)
        return report_application_failure(output_ref.error());

    auto service_roots = Json::array();
    for (const auto &root : roots) {
        Json value{{"kind", std::string{package_root_kind_name(root.kind)}},
                   {"groupName", root.group_name},
                   {"volumeName", root.volume_name},
                   {"objectName", root.object_name}};
        if (root.partition_index)
            value["partitionIndex"] = static_cast<std::uint32_t>(*root.partition_index);
        service_roots.push_back(std::move(value));
    }
    const Json input{{"source", file_ref_json(*source_ref)},
                     {"output", file_ref_json(*output_ref)},
                     {"roots", std::move(service_roots)},
                     {"overwrite", request.overwrite}};
    auto result = (*runtime)->invoke("package.export", input);
    if (!result)
        return report_application_failure(result.error());
    const auto &output = result->at("output");
    const app::FileRef effective_output{output.at("rootId").get<std::string>(),
                                        output.at("relativePath").get<std::string>()};
    auto output_path = (*runtime)->resolve_file(effective_output);
    if (!output_path)
        return report_application_failure(output_path.error());
    auto projected = schema::package_v1::project_package(*output_path, *result);
    if (!projected)
        return report_failure(projected.error());
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(*projected, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_package_summary(*projected, false);
    }
    return 0;
}

int run_package_inspect(const axk::cli::PackageReadRequest &request, bool verify_only) {
    const std::array paths{request.package};
    auto runtime = LocalOperationRuntime::create(paths);
    if (!runtime)
        return report_application_failure(runtime.error());
    auto package_ref = (*runtime)->file_ref(request.package);
    if (!package_ref)
        return report_application_failure(package_ref.error());
    const auto operation = verify_only ? "package.verify" : "package.inspect";
    const Json input{{"package", {{"fileRef", file_ref_json(*package_ref)}}}};
    auto result = (*runtime)->invoke(operation, input);
    if (!result)
        return report_application_failure(result.error());
    auto projected = schema::package_v1::project_package(request.package, *result);
    if (!projected)
        return report_failure(projected.error());
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(*projected, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_package_summary(*projected, verify_only);
    }
    return projected->valid ? 0 : 3;
}

int run_package_import(const axk::cli::PackageImportRequest &request) {
    axk::PackageImportRequest internal_request;
    internal_request.root_destinations.reserve(request.destinations.size());
    for (const auto &value : request.destinations) {
        auto destination = parse_destination(value);
        if (!destination)
            return report_failure(destination.error());
        internal_request.root_destinations.push_back(std::move(*destination));
    }
    if (request.rename_map) {
        auto renames = load_renames(*request.rename_map);
        if (!renames)
            return report_failure(renames.error());
        internal_request.policy.renames = std::move(*renames);
    }

    std::vector<std::filesystem::path> paths{request.target};
    paths.insert(paths.end(), request.packages.begin(), request.packages.end());
    if (request.output)
        paths.push_back(*request.output);
    auto runtime = LocalOperationRuntime::create(paths);
    if (!runtime)
        return report_application_failure(runtime.error());
    auto target_ref = (*runtime)->file_ref(request.target);
    if (!target_ref)
        return report_application_failure(target_ref.error());
    app::FileRef output_ref;
    if (request.output) {
        auto resolved = (*runtime)->file_ref(*request.output);
        if (!resolved)
            return report_application_failure(resolved.error());
        output_ref = std::move(*resolved);
    } else {
        output_ref = (*runtime)->scratch_file_ref("package-plan-output.tmp");
    }

    auto package_inputs = Json::array();
    for (const auto &path : request.packages) {
        auto package_ref = (*runtime)->file_ref(path);
        if (!package_ref)
            return report_application_failure(package_ref.error());
        package_inputs.push_back({{"fileRef", file_ref_json(*package_ref)}});
    }
    auto destinations = Json::array();
    for (const auto &destination : internal_request.root_destinations) {
        Json value{{"packageIndex", destination.package_index}, {"rootIndex", destination.root_index},
                   {"groupName", destination.group_name},       {"volumeName", destination.volume_name},
                   {"rawGroup", destination.raw_group},         {"rawVolume", destination.raw_volume},
                   {"create", destination.create_destination}};
        if (destination.partition_index)
            value["partitionIndex"] = static_cast<std::uint32_t>(*destination.partition_index);
        destinations.push_back(std::move(value));
    }
    auto renames = Json::array();
    for (const auto &rename : internal_request.policy.renames) {
        renames.push_back({{"packageIndex", rename.package_index},
                           {"nodeId", rename.node_id},
                           {"destinationName", rename.destination_name}});
    }
    const Json input{{"target", file_ref_json(*target_ref)},  {"output", file_ref_json(output_ref)},
                     {"packages", std::move(package_inputs)}, {"destinations", std::move(destinations)},
                     {"renames", std::move(renames)},         {"overwrite", request.overwrite}};
    auto plan = (*runtime)->invoke("package.plan_import", input);
    if (!plan)
        return report_application_failure(plan.error());

    std::optional<std::filesystem::path> output_path;
    std::optional<Json> application_result;
    if (request.apply && plan->at("valid").get<bool>()) {
        if (!request.output)
            return report_failure(argument_error("package import output path is required"));
        auto applied = (*runtime)->invoke("package.import", {{"planToken", plan->at("planToken").get<std::string>()}});
        if (!applied)
            return report_application_failure(applied.error());
        application_result = std::move(*applied);
        const auto &output = application_result->at("output");
        auto resolved = (*runtime)->resolve_file(
            {output.at("rootId").get<std::string>(), output.at("relativePath").get<std::string>()});
        if (!resolved)
            return report_application_failure(resolved.error());
        output_path = std::move(*resolved);
    }

    auto projected = schema::package_v1::project_plan(request.target, request.packages, *plan, output_path,
                                                      application_result ? &*application_result : nullptr);
    if (!projected)
        return report_failure(projected.error());
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(*projected, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_plan_summary(*projected);
    }
    return projected->valid ? 0 : 3;
}

} // namespace axk::cli::commands
