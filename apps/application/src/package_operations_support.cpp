#include "package_operations_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

#include "axklib/utf8.hpp"

namespace axk::app::package_operations_internal {

std::string normalized_path(const std::filesystem::path &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).generic_string();
}

axk::app::Error operation_error(std::string code, std::string message, std::optional<std::string> relative_path,
                                bool retryable) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context), retryable};
}

axk::app::Error core_error(const axk::Error &error, std::optional<std::string> relative_path) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = std::move(relative_path);
    std::string code = "package_operation_failed";
    if (error.code == axk::ErrorCode::operation_cancelled) {
        code = "operation_cancelled";
    } else if (error.code == axk::ErrorCode::transaction_stale) {
        code = "package_plan_stale";
    }
    return {std::move(code), error.message, std::move(context)};
}

axk::app::Result<void> write_reader(const std::filesystem::path &path, const axk::RandomAccessReader &reader) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(operation_error("package_read_failed", "could not create retained target staging"));
    std::vector<std::byte> buffer(
        static_cast<std::size_t>(std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, reader.size()))));
    for (std::uint64_t offset = 0U; offset < reader.size();) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.size() - offset));
        if (const auto read = reader.read_exact_at(offset, std::span{buffer}.first(count)); !read)
            return std::unexpected(core_error(read.error()));
        output.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(count));
        if (!output)
            return std::unexpected(operation_error("package_read_failed", "could not write retained target staging"));
        offset += count;
    }
    output.flush();
    if (!output)
        return std::unexpected(operation_error("package_read_failed", "could not flush retained target staging"));
    return {};
}

axk::app::Result<axk::app::FileRef> parse_file_ref(const Json &input, std::string_view field) {
    try {
        const auto &value = input.at(field);
        return axk::app::FileRef{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", std::format("{} must be a FileRef", field)));
    }
}

Json file_ref_json(const axk::app::FileRef &reference) {
    return {{"rootId", reference.root_id}, {"relativePath", reference.relative_path}};
}

axk::app::Result<PackageInput> parse_package_input(const Json &input) {
    try {
        if (!input.is_object())
            return std::unexpected(operation_error("invalid_request", "package input must be an object"));
        if (input.contains("fileRef") && !input.contains("uploadRef")) {
            const auto &value = input.at("fileRef");
            return PackageInput{
                axk::app::FileRef{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()}};
        }
        if (input.contains("uploadRef") && !input.contains("fileRef")) {
            const auto &value = input.at("uploadRef");
            return PackageInput{axk::app::UploadRef{value.at("uploadId").get<std::string>()}};
        }
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package input reference is malformed"));
    }
    return std::unexpected(
        operation_error("invalid_request", "package input must contain exactly one of fileRef or uploadRef"));
}

axk::app::Result<ResolvedPackage> resolve_package(const PackageInput &input, std::string_view owner_id,
                                                  const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    if (const auto *file = std::get_if<axk::app::FileRef>(&input.reference)) {
        auto opened = sandbox.open_file(*file);
        if (!opened)
            return std::unexpected(opened.error());
        return ResolvedPackage{std::move(opened->reader), std::move(opened->filename), std::nullopt};
    }
    const auto &upload = std::get<axk::app::UploadRef>(input.reference);
    auto snapshot = uploads.inspect(upload, owner_id);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    if (snapshot->kind != axk::app::UploadKind::package) {
        return std::unexpected(operation_error("upload_kind_mismatch", "upload is not a portable package"));
    }
    auto lease = uploads.lease(upload, owner_id);
    if (!lease)
        return std::unexpected(lease.error());
    auto reader = axk::FileReader::open(lease->path());
    if (!reader)
        return std::unexpected(operation_error("package_read_failed", reader.error().message));
    return ResolvedPackage{std::move(*reader), snapshot->filename, std::move(*lease)};
}

axk::app::Result<axk::PortablePackage> read_package(const ResolvedPackage &resolved, bool verify,
                                                    const axk::app::OperationContext &context) {
    auto package = verify ? axk::open_portable_package(*resolved.reader, resolved.filename, context.cancellation)
                          : axk::inspect_portable_package(*resolved.reader, resolved.filename, context.cancellation);
    if (!package)
        return std::unexpected(core_error(package.error()));
    return std::move(*package);
}

Json package_json(const axk::PortablePackage &package) {
    auto roots = Json::array();
    for (const auto &root : package.roots) {
        roots.push_back({{"kind", std::string{axk::package_root_kind_name(root.kind)}},
                         {"displayName", root.display_name},
                         {"nodeIds", root.node_ids}});
    }
    auto objects = Json::array();
    std::uint64_t total_payload_bytes{};
    for (const auto &node : package.nodes) {
        total_payload_bytes += node.payload_size_bytes;
        objects.push_back({{"nodeId", node.node_id},
                           {"objectType", node.object_type},
                           {"name", node.name},
                           {"payloadSizeBytes", node.payload_size_bytes},
                           {"payloadSha256", node.payload_sha256},
                           {"normalizedSha256", node.normalized_sha256},
                           {"semanticSha256", node.semantic_sha256 ? Json(*node.semantic_sha256) : Json{}},
                           {"audioSha256", node.audio_sha256 ? Json(*node.audio_sha256) : Json{}}});
    }
    auto relationships = Json::array();
    for (const auto &relationship : package.relationships) {
        relationships.push_back({{"edgeId", relationship.edge_id},
                                 {"sourceNodeId", relationship.source_node_id},
                                 {"targetNodeId", relationship.target_node_id},
                                 {"role", relationship.role},
                                 {"ordinal", relationship.ordinal}});
    }
    auto issues = Json::array();
    for (const auto &issue : package.issues)
        issues.push_back({{"code", issue.code}, {"message", issue.message}, {"fatal", issue.fatal}});
    return {{"schemaVersion", "1.0"},
            {"packageId", package.package_id},
            {"packageKind", std::string{axk::package_kind_name(package.kind)}},
            {"requiredExtension", std::string{axk::required_package_extension(package.kind)}},
            {"sourceMediaKind", package.source_media_kind},
            {"valid", std::ranges::none_of(package.issues, &axk::PackageIssue::fatal)},
            {"payloadsVerified", package.payloads_verified},
            {"totalPayloadBytes", total_payload_bytes},
            {"roots", std::move(roots)},
            {"objects", std::move(objects)},
            {"relationships", std::move(relationships)},
            {"relationshipCount", package.relationships.size()},
            {"issues", std::move(issues)}};
}

axk::app::Result<axk::PackageRootKind> parse_root_kind(std::string_view value) {
    if (value == "volume")
        return axk::PackageRootKind::volume;
    if (value == "program" || value == "prog")
        return axk::PackageRootKind::prog;
    if (value == "sbac" || value == "sample-bank")
        return axk::PackageRootKind::sbac;
    if (value == "sbnk" || value == "sample")
        return axk::PackageRootKind::sbnk;
    if (value == "smpl" || value == "wave-data")
        return axk::PackageRootKind::smpl;
    if (value == "sequence" || value == "sequ")
        return axk::PackageRootKind::sequ;
    return std::unexpected(operation_error("unsupported_package_root",
                                           "package root kind must be volume, program, sample-bank, sample, "
                                           "wave-data, sequence, sbac, sbnk, smpl, or sequ"));
}

axk::app::Result<std::vector<axk::PackageRootSelector>> parse_roots(const Json &input) {
    try {
        const auto &values = input.at("roots");
        if (!values.is_array() || values.empty() || values.size() > 1024U)
            return std::unexpected(operation_error("invalid_request", "roots must contain 1 to 1024 selectors"));
        std::vector<axk::PackageRootSelector> result;
        result.reserve(values.size());
        for (const auto &value : values) {
            auto kind = parse_root_kind(value.at("kind").get<std::string>());
            if (!kind)
                return std::unexpected(kind.error());
            axk::PackageRootSelector root;
            root.kind = *kind;
            if (value.contains("partitionIndex") && !value.at("partitionIndex").is_null()) {
                const auto partition = value.at("partitionIndex").get<std::uint32_t>();
                if (partition > std::numeric_limits<std::uint8_t>::max())
                    return std::unexpected(operation_error("invalid_request", "partition index is out of range"));
                root.partition_index = static_cast<std::uint8_t>(partition);
            }
            root.group_name = value.value("groupName", std::string{});
            root.volume_name = value.value("volumeName", std::string{});
            root.object_name = value.value("objectName", std::string{});
            if (root.kind == axk::PackageRootKind::volume && !root.object_name.empty())
                return std::unexpected(operation_error("invalid_request", "volume roots do not take objectName"));
            if (root.kind != axk::PackageRootKind::volume && root.object_name.empty()) {
                return std::unexpected(operation_error("invalid_request", "object package roots require objectName"));
            }
            result.push_back(std::move(root));
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package root selectors are malformed"));
    }
}

axk::app::Result<std::vector<axk::PackageRootSelector>> parse_session_export_roots(
    const Json &input, const std::unordered_map<std::string, std::string> &object_keys_by_id,
    const std::unordered_map<std::string, axk::app::ImageVolumeScopeIdentity> &volume_scopes_by_id) {
    try {
        const auto &values = input.at("roots");
        if (!values.is_array() || values.empty() || values.size() > 1024U)
            return std::unexpected(operation_error("invalid_request", "roots must contain 1 to 1024 selectors"));
        std::vector<axk::PackageRootSelector> result;
        result.reserve(values.size());
        std::set<std::string, std::less<>> identities;
        for (const auto &value : values) {
            const auto kind = value.at("kind").get<std::string>();
            axk::PackageRootSelector root;
            std::string identity;
            if (kind == "VOLUME") {
                const auto content_id = value.at("contentId").get<std::string>();
                if (content_id.empty() || value.size() != 2U) {
                    return std::unexpected(
                        operation_error("invalid_request", "volume roots require exactly kind and contentId"));
                }
                const auto volume = volume_scopes_by_id.find(content_id);
                if (volume == volume_scopes_by_id.end())
                    return std::unexpected(operation_error("content_not_found", "package volume does not exist"));
                root.kind = axk::PackageRootKind::volume;
                root.partition_index = volume->second.partition_index;
                root.volume_directory_id = volume->second.volume_directory_id;
                identity = "VOLUME\\0" + content_id;
            } else {
                if (kind == "PROGRAM")
                    root.kind = axk::PackageRootKind::prog;
                else if (kind == "SBAC")
                    root.kind = axk::PackageRootKind::sbac;
                else if (kind == "SBNK")
                    root.kind = axk::PackageRootKind::sbnk;
                else if (kind == "SMPL")
                    root.kind = axk::PackageRootKind::smpl;
                else if (kind == "SEQU")
                    root.kind = axk::PackageRootKind::sequ;
                else
                    return std::unexpected(operation_error("invalid_request", "package root kind is unsupported"));
                const auto object_id = value.at("objectId").get<std::string>();
                if (object_id.empty() || value.size() != 2U) {
                    return std::unexpected(
                        operation_error("invalid_request", "object roots require exactly kind and objectId"));
                }
                const auto object_key = object_keys_by_id.find(object_id);
                if (object_key == object_keys_by_id.end())
                    return std::unexpected(operation_error("object_not_found", "package root object does not exist"));
                identity = "OBJECT\\0" + object_id;
                root.object_key = object_key->second;
            }
            if (!identities.emplace(std::move(identity)).second)
                return std::unexpected(operation_error("invalid_request", "package roots must be unique"));
            result.push_back(std::move(root));
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package root selectors are malformed"));
    }
}

axk::app::Result<axk::PackageImportRequest> parse_import_request(const Json &input) {
    axk::PackageImportRequest result;
    try {
        if (!input.contains("destinations") || !input.at("destinations").is_array())
            return std::unexpected(operation_error("invalid_request", "destinations must be an array"));
        for (const auto &value : input.at("destinations")) {
            axk::PackageRootDestination destination;
            destination.package_index = value.at("packageIndex").get<std::size_t>();
            destination.root_index = value.at("rootIndex").get<std::size_t>();
            if (value.contains("partitionIndex") && !value.at("partitionIndex").is_null()) {
                const auto partition = value.at("partitionIndex").get<std::uint32_t>();
                if (partition > std::numeric_limits<std::uint8_t>::max())
                    return std::unexpected(operation_error("invalid_request", "partition index is out of range"));
                destination.partition_index = static_cast<std::uint8_t>(partition);
            }
            destination.group_name = value.value("groupName", std::string{});
            destination.volume_name = value.value("volumeName", std::string{});
            destination.raw_group = value.value("rawGroup", std::string{});
            destination.raw_volume = value.value("rawVolume", std::string{});
            destination.create_destination = value.value("create", false);
            result.root_destinations.push_back(std::move(destination));
        }
        if (input.contains("renames")) {
            if (!input.at("renames").is_array())
                return std::unexpected(operation_error("invalid_request", "renames must be an array"));
            for (const auto &value : input.at("renames")) {
                result.policy.renames.push_back({value.at("packageIndex").get<std::size_t>(),
                                                 value.at("nodeId").get<std::string>(),
                                                 value.at("destinationName").get<std::string>()});
            }
        }
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package import mappings are malformed"));
    }
    return result;
}

std::string target_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12-floppy";
    case axk::MediaKind::fat12_floppy_set:
        return "fat12-floppy-set";
    case axk::MediaKind::iso9660:
        return "iso9660";
    case axk::MediaKind::standalone_object:
        return "standalone-object";
    case axk::MediaKind::axk_object_directory:
        return "axk-object-directory";
    }
    return "unknown";
}

Json program_assignment_adjustments_json(std::span<const axk::PackageProgramAssignmentAdjustment> adjustments) {
    auto result = Json::array();
    for (const auto &adjustment : adjustments) {
        result.push_back(
            {{"adjustmentId", adjustment.adjustment_id},
             {"origin", std::string{axk::package_program_assignment_origin_name(adjustment.origin)}},
             {"packageIndex", adjustment.package_index ? Json(*adjustment.package_index) : Json{}},
             {"actionId", adjustment.action_id ? Json(*adjustment.action_id) : Json{}},
             {"existingObjectKey", adjustment.existing_object_key ? Json(*adjustment.existing_object_key) : Json{}},
             {"programSlot", adjustment.program_slot},
             {"programName", adjustment.program_name},
             {"assignmentOrdinal", adjustment.assignment_ordinal},
             {"targetObjectType", adjustment.target_object_type},
             {"targetName", adjustment.target_name},
             {"partitionIndex", adjustment.partition_index},
             {"groupName", adjustment.group_name},
             {"volumeName", adjustment.volume_name},
             {"rawGroup", adjustment.raw_group},
             {"rawVolume", adjustment.raw_volume},
             {"reasonCode", adjustment.reason_code},
             {"disposition", std::string{axk::package_program_assignment_disposition_name(adjustment.disposition)}}});
    }
    return result;
}

Json plan_json(const axk::PackageImportPlan &plan, std::string_view token, std::uint64_t expires_in_seconds) {
    auto warnings = Json::array();
    for (const auto &warning : plan.warnings)
        warnings.push_back({{"code", warning.code}, {"message", warning.message}, {"fatal", warning.fatal}});
    auto conflicts = Json::array();
    for (const auto &conflict : plan.conflicts) {
        conflicts.push_back({{"code", conflict.code},
                             {"message", conflict.message},
                             {"packageIndex", conflict.package_index ? Json(*conflict.package_index) : Json{}},
                             {"rootIndex", conflict.root_index ? Json(*conflict.root_index) : Json{}},
                             {"packageId", conflict.package_id},
                             {"nodeId", conflict.node_id},
                             {"partitionIndex", conflict.partition_index ? Json(*conflict.partition_index) : Json{}},
                             {"groupName", conflict.group_name},
                             {"volumeName", conflict.volume_name},
                             {"rawGroup", conflict.raw_group},
                             {"rawVolume", conflict.raw_volume}});
    }
    auto actions = Json::array();
    for (const auto &object : plan.objects) {
        auto names = Json::array();
        for (const auto action : object.actions)
            names.push_back(std::string{axk::package_import_action_name(action)});
        actions.push_back(
            {{"actionId", object.action_id},
             {"packageIndex", object.package_index},
             {"rootIndex", object.root_index},
             {"packageId", object.package_id},
             {"nodeId", object.node_id},
             {"objectType", object.object_type},
             {"sourceName", object.source_name},
             {"destinationName", object.destination_name},
             {"partitionIndex", object.partition_index},
             {"groupName", object.group_name},
             {"volumeName", object.volume_name},
             {"rawGroup", object.raw_group},
             {"rawVolume", object.raw_volume},
             {"actions", std::move(names)},
             {"canonicalActionId", object.canonical_action_id ? Json(*object.canonical_action_id) : Json{}},
             {"targetSfsId", object.target_sfs_id ? Json(*object.target_sfs_id) : Json{}},
             {"targetWaveDataReferenceValue",
              object.target_wave_data_reference_value ? Json(*object.target_wave_data_reference_value) : Json{}}});
    }
    auto allocation = Json::array();
    for (const auto &item : plan.allocation) {
        allocation.push_back({{"partitionIndex", item.partition_index},
                              {"groupName", item.group_name},
                              {"volumeName", item.volume_name},
                              {"rawGroup", item.raw_group},
                              {"rawVolume", item.raw_volume},
                              {"insertedObjectCount", item.inserted_object_count},
                              {"reusedObjectCount", item.reused_object_count},
                              {"blockedObjectCount", item.blocked_object_count},
                              {"payloadClusters", item.payload_clusters},
                              {"payloadSectors", item.payload_sectors},
                              {"continuationClusters", item.continuation_clusters},
                              {"directoryGrowthBytes", item.directory_growth_bytes},
                              {"directoryGrowthClusters", item.directory_growth_clusters},
                              {"directoryContinuationClusters", item.directory_continuation_clusters},
                              {"infrastructureClusters", item.infrastructure_clusters},
                              {"additionalAllocatedBytes", item.additional_allocated_bytes},
                              {"remainingObjectIds", item.remaining_object_ids},
                              {"remainingClusters", item.remaining_clusters},
                              {"projectedImageSectors", item.projected_image_sectors},
                              {"projectedImageSizeBytes", item.projected_image_size_bytes}});
    }
    return {{"schemaVersion", "1.0"},
            {"planToken", token},
            {"expiresInSeconds", expires_in_seconds},
            {"planId", plan.plan_id},
            {"targetKind", target_kind_name(plan.target_kind)},
            {"targetSnapshotId", plan.target_snapshot_id},
            {"valid", plan.valid()},
            {"warnings", std::move(warnings)},
            {"conflicts", std::move(conflicts)},
            {"actions", std::move(actions)},
            {"programAssignmentAdjustments", program_assignment_adjustments_json(plan.program_assignment_adjustments)},
            {"allocation", std::move(allocation)}};
}

void cleanup_session_plans(SessionPackageOperationState &state, Clock::time_point now) {
    for (auto current = state.plans.begin(); current != state.plans.end();) {
        if (!current->second->claimed && current->second->expires_at <= now)
            current = state.plans.erase(current);
        else
            ++current;
    }
}

axk::app::Result<std::uint64_t> retained_package_bytes(const axk::PortablePackage &package) {
    std::uint64_t total{};
    for (const auto &node : package.nodes) {
        if (node.raw_payload.size() > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::unexpected(
                operation_error("package_read_failed", "retained package payload size exceeds supported bounds"));
        }
        total += node.raw_payload.size();
    }
    return total;
}

std::uint64_t retained_session_package_bytes(const SessionPackageOperationState &state) {
    std::set<const VerifiedPackageSnapshot *> counted;
    std::uint64_t total{};
    for (const auto &[token, record] : state.plans) {
        static_cast<void>(token);
        if (record->package_snapshot && counted.emplace(record->package_snapshot.get()).second)
            total += record->package_snapshot->retained_payload_bytes;
    }
    return total;
}

axk::app::Result<SessionPackagePlanClaim> claim_session_plan(const std::shared_ptr<SessionPackageOperationState> &state,
                                                             std::string_view token, std::string_view owner_id) {
    std::lock_guard lock{state->mutex};
    cleanup_session_plans(*state, Clock::now());
    const auto found = state->plans.find(std::string{token});
    if (found == state->plans.end() || found->second->owner_id != owner_id) {
        return std::unexpected(operation_error("package_plan_not_found", "package import plan is absent or expired"));
    }
    if (found->second->claimed)
        return std::unexpected(operation_error("package_plan_in_use", "package import plan is already being applied"));
    found->second->claimed = true;
    return SessionPackagePlanClaim{state, found->second};
}

axk::app::Result<std::pair<std::string, std::uint64_t>> parse_session_identity(const Json &input) {
    try {
        return std::pair{input.at("imageId").get<std::string>(), input.at("expectedRevision").get<std::uint64_t>()};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "imageId and expectedRevision are required"));
    }
}

axk::app::Result<axk::PackageImportRequest> parse_session_import_request(const Json &input,
                                                                         const axk::PortablePackage &package) {
    axk::PackageImportRequest request;
    try {
        const auto partition = input.at("partitionIndex").get<std::uint32_t>();
        const auto volume = input.at("volumeName").get<std::string>();
        if (partition > std::numeric_limits<std::uint8_t>::max() || volume.empty())
            return std::unexpected(operation_error("invalid_request", "package destination is invalid"));
        for (std::size_t root_index = 0U; root_index < package.roots.size(); ++root_index) {
            request.root_destinations.push_back(
                {0U, root_index, static_cast<std::uint8_t>(partition), {}, volume, {}, {}, false});
        }
        if (input.contains("renames")) {
            for (const auto &rename : input.at("renames")) {
                request.policy.renames.push_back(
                    {0U, rename.at("nodeId").get<std::string>(), rename.at("destinationName").get<std::string>()});
            }
        }
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package destination or renames are malformed"));
    }
    return request;
}

Json session_import_result(const SessionPackagePlanRecord &record, const axk::app::ImageSessionSummary &summary,
                           bool applied) {
    auto result = plan_json(record.plan, record.token, 0U);
    result.erase("planToken");
    result.erase("expiresInSeconds");
    result.erase("valid");
    result.erase("warnings");
    result.erase("conflicts");
    result["imageId"] = record.image_id;
    result["revision"] = summary.revision;
    result["objectCount"] = summary.object_count;
    result["applied"] = applied;
    return result;
}

axk::app::Result<Json> read_operation(const Json &input, const axk::app::OperationContext &context,
                                      const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads, bool verify) {
    auto source = parse_package_input(input.at("package"));
    if (!source)
        return std::unexpected(source.error());
    auto resolved = resolve_package(*source, context.owner_id, sandbox, uploads);
    if (!resolved)
        return std::unexpected(resolved.error());
    auto package = read_package(*resolved, verify, context);
    if (!package)
        return std::unexpected(package.error());
    return package_json(*package);
}

} // namespace axk::app::package_operations_internal
