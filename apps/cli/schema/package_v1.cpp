#include "commands/package_projection.hpp"

#include <iterator>
#include <ranges>

#include <nlohmann/json.hpp>

#include "axklib/utf8.hpp"

namespace axk::cli::schema::package_v1 {
namespace {

using OrderedJson = nlohmann::ordered_json;

Error serialization_error(const nlohmann::json::exception &error) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                      std::string{"could not serialize package CLI JSON: "} + error.what());
}

OrderedJson optional_string(const std::optional<std::string> &value) {
    return value ? OrderedJson(*value) : OrderedJson(nullptr);
}

template <typename T> OrderedJson optional_number(const std::optional<T> &value) {
    return value ? OrderedJson(*value) : OrderedJson(nullptr);
}

template <typename T> std::optional<T> optional_value(const nlohmann::json &value) {
    return value.is_null() ? std::nullopt : std::optional<T>{value.get<T>()};
}

OrderedJson issue_json(const IssueOutput &issue) {
    return {{"code", issue.code}, {"message", issue.message}, {"fatal", issue.fatal}};
}

} // namespace

PackageOutput project_package(const std::filesystem::path &path, const PortablePackage &package) {
    PackageOutput result;
    result.path_utf8 = text::path_to_utf8(path);
    result.package_id = package.package_id;
    result.package_kind = package_kind_name(package.kind);
    result.required_extension = required_package_extension(package.kind);
    result.source_media_kind = package.source_media_kind;
    result.valid = std::ranges::none_of(package.issues, &PackageIssue::fatal);
    result.payloads_verified = package.payloads_verified;
    result.relationship_count = package.relationships.size();
    result.roots.reserve(package.roots.size());
    for (const auto &root : package.roots) {
        result.roots.push_back({std::string{package_root_kind_name(root.kind)}, root.display_name, root.node_ids});
    }
    result.objects.reserve(package.nodes.size());
    for (const auto &node : package.nodes) {
        result.objects.push_back({node.node_id, node.object_type, node.name, node.payload_sha256,
                                  node.normalized_sha256, node.semantic_sha256, node.audio_sha256});
    }
    result.issues.reserve(package.issues.size());
    std::ranges::transform(package.issues, std::back_inserter(result.issues), [](const PackageIssue &issue) {
        return IssueOutput{issue.code, issue.message, issue.fatal};
    });
    return result;
}

Result<PackageOutput> project_package(const std::filesystem::path &path, const nlohmann::json &service_result) {
    try {
        PackageOutput result;
        result.path_utf8 = text::path_to_utf8(path);
        result.package_id = service_result.at("packageId").get<std::string>();
        result.package_kind = service_result.at("packageKind").get<std::string>();
        result.required_extension = service_result.at("requiredExtension").get<std::string>();
        result.source_media_kind = service_result.at("sourceMediaKind").get<std::string>();
        result.valid = service_result.at("valid").get<bool>();
        result.payloads_verified = service_result.at("payloadsVerified").get<bool>();
        result.relationship_count = service_result.at("relationshipCount").get<std::uint64_t>();
        for (const auto &root : service_result.at("roots")) {
            result.roots.push_back({root.at("kind").get<std::string>(), root.at("displayName").get<std::string>(),
                                    root.at("nodeIds").get<std::vector<std::string>>()});
        }
        for (const auto &node : service_result.at("objects")) {
            result.objects.push_back(
                {node.at("nodeId").get<std::string>(), node.at("objectType").get<std::string>(),
                 node.at("name").get<std::string>(), node.at("payloadSha256").get<std::string>(),
                 node.at("normalizedSha256").get<std::string>(),
                 node.at("semanticSha256").is_null() ? std::nullopt
                                                     : std::optional{node.at("semanticSha256").get<std::string>()},
                 node.at("audioSha256").is_null() ? std::nullopt
                                                  : std::optional{node.at("audioSha256").get<std::string>()}});
        }
        for (const auto &issue : service_result.at("issues")) {
            result.issues.push_back({issue.at("code").get<std::string>(), issue.at("message").get<std::string>(),
                                     issue.at("fatal").get<bool>()});
        }
        return result;
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

PlanOutput project_plan(const std::filesystem::path &target, const std::vector<std::filesystem::path> &package_paths,
                        const PackageImportPlan &plan, const std::optional<PackageImportReport> &report) {
    PlanOutput result;
    result.target_path_utf8 = text::path_to_utf8(target);
    result.plan_id = plan.plan_id;
    result.target_kind = plan.target_kind == MediaKind::sfs            ? "sfs"
                         : plan.target_kind == MediaKind::fat12_floppy ? "fat12-floppy"
                         : plan.target_kind == MediaKind::iso9660      ? "iso9660"
                                                                       : "standalone-object";
    result.target_snapshot_id = plan.target_snapshot_id;
    result.valid = plan.valid();
    result.package_paths_utf8.reserve(package_paths.size());
    std::ranges::transform(package_paths, std::back_inserter(result.package_paths_utf8),
                           [](const auto &path) { return text::path_to_utf8(path); });
    result.warnings.reserve(plan.warnings.size());
    std::ranges::transform(plan.warnings, std::back_inserter(result.warnings), [](const PackageIssue &issue) {
        return IssueOutput{issue.code, issue.message, issue.fatal};
    });
    result.conflicts.reserve(plan.conflicts.size());
    for (const auto &conflict : plan.conflicts) {
        result.conflicts.push_back({
            conflict.code,
            conflict.message,
            conflict.package_index,
            conflict.root_index,
            conflict.package_id,
            conflict.node_id,
            conflict.partition_index,
            conflict.group_name,
            conflict.volume_name,
            conflict.raw_group,
            conflict.raw_volume,
        });
    }
    result.objects.reserve(plan.objects.size());
    for (const auto &object : plan.objects) {
        ActionOutput projected;
        projected.action_id = object.action_id;
        projected.package_index = object.package_index;
        projected.root_index = object.root_index;
        projected.package_id = object.package_id;
        projected.node_id = object.node_id;
        projected.object_type = object.object_type;
        projected.source_name = object.source_name;
        projected.destination_name = object.destination_name;
        projected.partition_index = object.partition_index;
        projected.group_name = object.group_name;
        projected.volume_name = object.volume_name;
        projected.raw_group = object.raw_group;
        projected.raw_volume = object.raw_volume;
        projected.canonical_action_id = object.canonical_action_id;
        projected.target_sfs_id = object.target_sfs_id;
        projected.target_wave_data_reference_value = object.target_wave_data_reference_value;
        projected.actions.reserve(object.actions.size());
        std::ranges::transform(
            object.actions, std::back_inserter(projected.actions),
            [](PackageImportObjectAction action) { return std::string{package_import_action_name(action)}; });
        result.objects.push_back(std::move(projected));
    }
    result.allocation.reserve(plan.allocation.size());
    for (const auto &allocation : plan.allocation) {
        result.allocation.push_back({
            allocation.partition_index,
            allocation.group_name,
            allocation.volume_name,
            allocation.raw_group,
            allocation.raw_volume,
            allocation.inserted_object_count,
            allocation.reused_object_count,
            allocation.payload_clusters,
            allocation.payload_sectors,
            allocation.continuation_clusters,
            allocation.directory_growth_bytes,
            allocation.remaining_object_ids,
            allocation.remaining_clusters,
            allocation.projected_image_sectors,
            allocation.projected_image_size_bytes,
        });
    }
    if (report) {
        result.result = ImportResultOutput{
            text::path_to_utf8(report->output_path),
            report->source_snapshot_id,
            report->output_snapshot_id,
            report->applied,
        };
    }
    return result;
}

Result<PlanOutput> project_plan(const std::filesystem::path &target,
                                const std::vector<std::filesystem::path> &package_paths,
                                const nlohmann::json &service_plan,
                                const std::optional<std::filesystem::path> &output_path,
                                const nlohmann::json *service_result) {
    try {
        PlanOutput result;
        result.target_path_utf8 = text::path_to_utf8(target);
        result.package_paths_utf8.reserve(package_paths.size());
        std::ranges::transform(package_paths, std::back_inserter(result.package_paths_utf8),
                               [](const auto &path) { return text::path_to_utf8(path); });
        result.plan_id = service_plan.at("planId").get<std::string>();
        result.target_kind = service_plan.at("targetKind").get<std::string>();
        result.target_snapshot_id = service_plan.at("targetSnapshotId").get<std::string>();
        result.valid = service_plan.at("valid").get<bool>();
        for (const auto &warning : service_plan.at("warnings")) {
            result.warnings.push_back({warning.at("code").get<std::string>(), warning.at("message").get<std::string>(),
                                       warning.at("fatal").get<bool>()});
        }
        for (const auto &conflict : service_plan.at("conflicts")) {
            result.conflicts.push_back({
                conflict.at("code").get<std::string>(),
                conflict.at("message").get<std::string>(),
                optional_value<std::uint64_t>(conflict.at("packageIndex")),
                optional_value<std::uint64_t>(conflict.at("rootIndex")),
                conflict.at("packageId").get<std::string>(),
                conflict.at("nodeId").get<std::string>(),
                optional_value<std::uint32_t>(conflict.at("partitionIndex")),
                conflict.at("groupName").get<std::string>(),
                conflict.at("volumeName").get<std::string>(),
                conflict.at("rawGroup").get<std::string>(),
                conflict.at("rawVolume").get<std::string>(),
            });
        }
        for (const auto &object : service_plan.at("actions")) {
            result.objects.push_back({
                object.at("actionId").get<std::string>(),
                object.at("packageIndex").get<std::uint64_t>(),
                object.at("rootIndex").get<std::uint64_t>(),
                object.at("packageId").get<std::string>(),
                object.at("nodeId").get<std::string>(),
                object.at("objectType").get<std::string>(),
                object.at("sourceName").get<std::string>(),
                object.at("destinationName").get<std::string>(),
                object.at("partitionIndex").get<std::uint32_t>(),
                object.at("groupName").get<std::string>(),
                object.at("volumeName").get<std::string>(),
                object.at("rawGroup").get<std::string>(),
                object.at("rawVolume").get<std::string>(),
                object.at("actions").get<std::vector<std::string>>(),
                optional_value<std::string>(object.at("canonicalActionId")),
                optional_value<std::uint32_t>(object.at("targetSfsId")),
                optional_value<std::uint32_t>(object.at("targetWaveDataReferenceValue")),
            });
        }
        for (const auto &allocation : service_plan.at("allocation")) {
            result.allocation.push_back({
                allocation.at("partitionIndex").get<std::uint32_t>(),
                allocation.at("groupName").get<std::string>(),
                allocation.at("volumeName").get<std::string>(),
                allocation.at("rawGroup").get<std::string>(),
                allocation.at("rawVolume").get<std::string>(),
                allocation.at("insertedObjectCount").get<std::uint64_t>(),
                allocation.at("reusedObjectCount").get<std::uint64_t>(),
                allocation.at("payloadClusters").get<std::uint64_t>(),
                allocation.at("payloadSectors").get<std::uint64_t>(),
                allocation.at("continuationClusters").get<std::uint64_t>(),
                allocation.at("directoryGrowthBytes").get<std::uint64_t>(),
                allocation.at("remainingObjectIds").get<std::uint64_t>(),
                allocation.at("remainingClusters").get<std::uint64_t>(),
                allocation.at("projectedImageSectors").get<std::uint64_t>(),
                allocation.at("projectedImageSizeBytes").get<std::uint64_t>(),
            });
        }
        if (service_result != nullptr) {
            if (!output_path)
                return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                                                  "package import result requires an output path")};
            result.result = ImportResultOutput{
                text::path_to_utf8(*output_path), service_result->at("sourceSnapshotId").get<std::string>(),
                service_result->at("outputSnapshotId").get<std::string>(), service_result->at("applied").get<bool>()};
        }
        return result;
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

Result<std::string> serialize(const PackageOutput &output, bool pretty) {
    try {
        auto roots = OrderedJson::array();
        for (const auto &root : output.roots)
            roots.push_back({{"kind", root.kind}, {"display_name", root.display_name}, {"node_ids", root.node_ids}});
        auto objects = OrderedJson::array();
        for (const auto &object : output.objects) {
            objects.push_back({
                {"node_id", object.node_id},
                {"object_type", object.object_type},
                {"name", object.name},
                {"payload_sha256", object.payload_sha256},
                {"normalized_sha256", object.normalized_sha256},
                {"semantic_sha256", optional_string(object.semantic_sha256)},
                {"audio_sha256", optional_string(object.audio_sha256)},
            });
        }
        auto issues = OrderedJson::array();
        for (const auto &issue : output.issues)
            issues.push_back(issue_json(issue));
        return OrderedJson{
            {"schema_version", schema_version},
            {"path", output.path_utf8},
            {"package_id", output.package_id},
            {"package_kind", output.package_kind},
            {"required_extension", output.required_extension},
            {"source_media_kind", output.source_media_kind},
            {"valid", output.valid},
            {"payloads_verified", output.payloads_verified},
            {"roots", std::move(roots)},
            {"objects", std::move(objects)},
            {"relationship_count", output.relationship_count},
            {"issues", std::move(issues)},
        }
            .dump(pretty ? 2 : -1);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

Result<std::string> serialize(const PlanOutput &output, bool pretty) {
    try {
        auto warnings = OrderedJson::array();
        for (const auto &warning : output.warnings)
            warnings.push_back(issue_json(warning));
        auto conflicts = OrderedJson::array();
        for (const auto &conflict : output.conflicts) {
            conflicts.push_back({
                {"code", conflict.code},
                {"message", conflict.message},
                {"package_index", optional_number(conflict.package_index)},
                {"root_index", optional_number(conflict.root_index)},
                {"package_id", conflict.package_id},
                {"node_id", conflict.node_id},
                {"partition_index", optional_number(conflict.partition_index)},
                {"group_name", conflict.group_name},
                {"volume_name", conflict.volume_name},
                {"raw_group", conflict.raw_group},
                {"raw_volume", conflict.raw_volume},
            });
        }
        auto objects = OrderedJson::array();
        for (const auto &object : output.objects) {
            objects.push_back({
                {"action_id", object.action_id},
                {"package_index", object.package_index},
                {"root_index", object.root_index},
                {"package_id", object.package_id},
                {"node_id", object.node_id},
                {"object_type", object.object_type},
                {"source_name", object.source_name},
                {"destination_name", object.destination_name},
                {"partition_index", object.partition_index},
                {"group_name", object.group_name},
                {"volume_name", object.volume_name},
                {"raw_group", object.raw_group},
                {"raw_volume", object.raw_volume},
                {"actions", object.actions},
                {"canonical_action_id", optional_string(object.canonical_action_id)},
                {"target_sfs_id", optional_number(object.target_sfs_id)},
                {"target_wave_data_reference_value", optional_number(object.target_wave_data_reference_value)},
            });
        }
        auto allocation = OrderedJson::array();
        for (const auto &item : output.allocation) {
            allocation.push_back({
                {"partition_index", item.partition_index},
                {"group_name", item.group_name},
                {"volume_name", item.volume_name},
                {"raw_group", item.raw_group},
                {"raw_volume", item.raw_volume},
                {"inserted_object_count", item.inserted_object_count},
                {"reused_object_count", item.reused_object_count},
                {"payload_clusters", item.payload_clusters},
                {"payload_sectors", item.payload_sectors},
                {"continuation_clusters", item.continuation_clusters},
                {"directory_growth_bytes", item.directory_growth_bytes},
                {"remaining_object_ids", item.remaining_object_ids},
                {"remaining_clusters", item.remaining_clusters},
                {"projected_image_sectors", item.projected_image_sectors},
                {"projected_image_size_bytes", item.projected_image_size_bytes},
            });
        }
        const auto result = output.result ? OrderedJson{{"output_path", output.result->output_path_utf8},
                                                        {"source_snapshot_id", output.result->source_snapshot_id},
                                                        {"output_snapshot_id", output.result->output_snapshot_id},
                                                        {"applied", output.result->applied}}
                                          : OrderedJson(nullptr);
        return OrderedJson{
            {"schema_version", schema_version},
            {"target_path", output.target_path_utf8},
            {"package_paths", output.package_paths_utf8},
            {"plan_id", output.plan_id},
            {"target_kind", output.target_kind},
            {"target_snapshot_id", output.target_snapshot_id},
            {"valid", output.valid},
            {"warnings", std::move(warnings)},
            {"conflicts", std::move(conflicts)},
            {"objects", std::move(objects)},
            {"allocation", std::move(allocation)},
            {"result", result},
        }
            .dump(pretty ? 2 : -1);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

} // namespace axk::cli::schema::package_v1
