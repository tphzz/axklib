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

#include "requests.hpp"
#include "schema/package_v1.hpp"
#include "support.hpp"

#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/utf8.hpp"

namespace axk::cli::commands {
namespace {

using Json = nlohmann::json;

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
    auto media = open_media(request.source);
    if (!media)
        return report_failure(media.error());
    std::vector<PackageRootSelector> roots;
    roots.reserve(request.roots.size());
    for (const auto &value : request.roots) {
        auto root = parse_root(value, request);
        if (!root)
            return report_failure(root.error());
        roots.push_back(std::move(*root));
    }
    auto published = export_portable_package(*media, roots, request.output, request.overwrite);
    if (!published)
        return report_failure(published.error());
    auto package = open_portable_package(published->output_path);
    if (!package)
        return report_failure(package.error());
    const auto projected = schema::package_v1::project_package(published->output_path, *package);
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(projected, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_package_summary(projected, false);
    }
    return 0;
}

int run_package_inspect(const axk::cli::PackageReadRequest &request, bool verify_only) {
    auto package = verify_only ? open_portable_package(request.package) : inspect_portable_package(request.package);
    if (!package)
        return report_failure(package.error());
    if (verify_only) {
        if (const auto verified = verify_portable_package(*package); !verified)
            return report_failure(verified.error());
    }
    const auto projected = schema::package_v1::project_package(request.package, *package);
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(projected, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_package_summary(projected, verify_only);
    }
    return projected.valid ? 0 : 3;
}

int run_package_import(const axk::cli::PackageImportRequest &request) {
    std::vector<PortablePackage> packages;
    packages.reserve(request.packages.size());
    for (const auto &path : request.packages) {
        auto package = open_portable_package(path);
        if (!package)
            return report_failure(package.error());
        packages.push_back(std::move(*package));
    }
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
    auto plan = plan_package_import(request.target, packages, internal_request);
    if (!plan)
        return report_failure(plan.error());

    std::optional<PackageImportReport> report;
    if (request.apply && plan->valid()) {
        if (!request.output)
            return report_failure(argument_error("package import output path is required"));
        auto applied = apply_package_import(request.target, packages, *plan, *request.output, request.overwrite);
        if (!applied)
            return report_failure(applied.error());
        report = std::move(*applied);
    }
    const auto projected = schema::package_v1::project_plan(request.target, request.packages, *plan, report);
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(projected, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_plan_summary(projected);
    }
    return plan->valid() ? 0 : 3;
}

} // namespace axk::cli::commands
