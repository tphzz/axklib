#include "handlers.hpp"

#include <array>
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

Error argument_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::internal, std::move(message));
}

Result<PackageRootKind> parse_root_kind(std::string_view value) {
    if (value == "volume")
        return PackageRootKind::volume;
    if (value == "program" || value == "prog")
        return PackageRootKind::prog;
    if (value == "sbac" || value == "sample-bank")
        return PackageRootKind::sbac;
    if (value == "sbnk" || value == "sample")
        return PackageRootKind::sbnk;
    if (value == "smpl" || value == "wave-data")
        return PackageRootKind::smpl;
    if (value == "sequence" || value == "sequ")
        return PackageRootKind::sequ;
    return std::unexpected{argument_error("package root kind must be volume, program, sample-bank, sample, "
                                          "wave-data, sequence, sbac, sbnk, smpl, or sequ")};
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

Result<std::vector<PackageProgramSlotAssignment>> load_program_slot_assignments(const std::filesystem::path &path) {
    std::ifstream input{path};
    if (!input)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open Program slot map")};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input && !input.eof())
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not read Program slot map")};
    const auto text_value = buffer.str();
    if (!text::is_valid_utf8(text_value))
        return std::unexpected{argument_error("Program slot map is not valid UTF-8")};
    try {
        const auto array = Json::parse(text_value);
        if (!array.is_array())
            return std::unexpected{argument_error("Program slot map must be a JSON array")};
        std::vector<PackageProgramSlotAssignment> result;
        result.reserve(array.size());
        static const std::set<std::string, std::less<>> fields{"package", "node_id", "slot"};
        for (const auto &item : array) {
            if (!has_only_fields(item, fields) || !item.contains("package") || !item.contains("node_id") ||
                !item.contains("slot")) {
                return std::unexpected{
                    argument_error("each Program slot assignment must contain only package, node_id, and slot")};
            }
            const auto slot = item.at("slot").get<std::uint32_t>();
            if (slot < 1U || slot > 128U)
                return std::unexpected{argument_error("Program destination slots must be between 1 and 128")};
            result.push_back({item.at("package").get<std::size_t>(), item.at("node_id").get<std::string>(),
                              static_cast<std::uint8_t>(slot)});
        }
        return result;
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{argument_error(std::string{"invalid Program slot map JSON: "} + error.what())};
    }
}

Result<std::vector<PackageOpaqueSequenceDecision>> load_opaque_sequence_decisions(const std::filesystem::path &path) {
    std::ifstream input{path};
    if (!input)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open opaque Sequence map")};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input && !input.eof())
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not read opaque Sequence map")};
    const auto text_value = buffer.str();
    if (!text::is_valid_utf8(text_value))
        return std::unexpected{argument_error("opaque Sequence map is not valid UTF-8")};
    try {
        const auto array = Json::parse(text_value);
        if (!array.is_array())
            return std::unexpected{argument_error("opaque Sequence map must be a JSON array")};
        std::vector<PackageOpaqueSequenceDecision> result;
        result.reserve(array.size());
        static const std::set<std::string, std::less<>> fields{"package", "node_id", "action"};
        for (const auto &item : array) {
            if (!has_only_fields(item, fields) || !item.contains("package") || !item.contains("node_id") ||
                !item.contains("action")) {
                return std::unexpected{
                    argument_error("each opaque Sequence decision must contain only package, node_id, and action")};
            }
            const auto action = item.at("action").get<std::string>();
            if (action != "preserve-unchanged" && action != "skip")
                return std::unexpected{argument_error("opaque Sequence action must be preserve-unchanged or skip")};
            result.push_back({item.at("package").get<std::size_t>(), item.at("node_id").get<std::string>(),
                              action == "preserve-unchanged" ? PackageOpaqueSequenceAction::preserve_unchanged
                                                             : PackageOpaqueSequenceAction::skip});
        }
        return result;
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{argument_error(std::string{"invalid opaque Sequence map JSON: "} + error.what())};
    }
}

void print_package_summary(const schema::package_v1::PackageOutput &output, bool verify_only) {
    std::cout << output.path_utf8 << '\t' << (output.valid ? "valid" : "invalid") << "\tkind=" << output.package_kind
              << "\tpackage_id=" << output.package_id << "\troots=" << output.roots.size()
              << "\tobjects=" << output.objects.size() << "\tpayload_bytes=" << output.total_payload_bytes
              << "\trelationships=" << output.relationship_count;
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
    for (const auto &allocation : output.allocation) {
        std::cout << "allocation\tpartition=" << allocation.partition_index << "\tvolume=" << allocation.volume_name
                  << "\tinsert=" << allocation.inserted_object_count << "\treuse=" << allocation.reused_object_count
                  << "\tblocked=" << allocation.blocked_object_count
                  << "\tadditional_bytes=" << allocation.additional_allocated_bytes << '\n';
    }
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

    auto result = (*runtime)->package_export(*source_ref, *output_ref, roots, request.overwrite);
    if (!result)
        return report_application_failure(result.error());
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(*result, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_package_summary(*result, false);
    }
    return exit_code(ExitStatus::success);
}

int run_package_inspect(const axk::cli::PackageReadRequest &request, bool verify_only) {
    const std::array paths{request.package};
    auto runtime = LocalOperationRuntime::create(paths);
    if (!runtime)
        return report_application_failure(runtime.error());
    auto package_ref = (*runtime)->file_ref(request.package);
    if (!package_ref)
        return report_application_failure(package_ref.error());
    auto result = (*runtime)->package_inspect(request.package, *package_ref, verify_only);
    if (!result)
        return report_application_failure(result.error());
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(*result, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_package_summary(*result, verify_only);
    }
    return exit_code(result->valid ? ExitStatus::success : ExitStatus::diagnostics);
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
    if (request.program_slot_map) {
        auto assignments = load_program_slot_assignments(*request.program_slot_map);
        if (!assignments)
            return report_failure(assignments.error());
        internal_request.policy.program_slot_assignments = std::move(*assignments);
    }
    if (request.opaque_sequence_map) {
        auto decisions = load_opaque_sequence_decisions(*request.opaque_sequence_map);
        if (!decisions)
            return report_failure(decisions.error());
        internal_request.policy.opaque_sequence_decisions = std::move(*decisions);
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

    std::vector<app::FileRef> package_inputs;
    for (const auto &path : request.packages) {
        auto package_ref = (*runtime)->file_ref(path);
        if (!package_ref)
            return report_application_failure(package_ref.error());
        package_inputs.push_back(std::move(*package_ref));
    }

    if (request.apply && !request.output)
        return report_failure(argument_error("package import output path is required"));
    auto projected = (*runtime)->package_import(request.target, request.packages, *target_ref, output_ref,
                                                package_inputs, internal_request, request.apply, request.overwrite);
    if (!projected)
        return report_application_failure(projected.error());
    if (request.format == "json") {
        auto serialized = schema::package_v1::serialize(*projected, false);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
    } else {
        print_plan_summary(*projected);
    }
    return exit_code(projected->valid ? ExitStatus::success : ExitStatus::diagnostics);
}

} // namespace axk::cli::commands
