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
        if (input.contains("programSlotAssignments")) {
            if (!input.at("programSlotAssignments").is_array())
                return std::unexpected(operation_error("invalid_request", "programSlotAssignments must be an array"));
            for (const auto &value : input.at("programSlotAssignments")) {
                const auto slot = value.at("destinationSlot").get<std::uint32_t>();
                if (slot < 1U || slot > 128U)
                    return std::unexpected(operation_error("invalid_request", "Program slot is out of range"));
                result.policy.program_slot_assignments.push_back({value.at("packageIndex").get<std::size_t>(),
                                                                  value.at("nodeId").get<std::string>(),
                                                                  static_cast<std::uint8_t>(slot)});
            }
        }
        if (input.contains("opaqueSequenceDecisions")) {
            if (!input.at("opaqueSequenceDecisions").is_array()) {
                return std::unexpected(operation_error("invalid_request", "opaqueSequenceDecisions must be an array"));
            }
            for (const auto &value : input.at("opaqueSequenceDecisions")) {
                const auto action = value.at("action").get<std::string>();
                if (action != "preserve-unchanged" && action != "skip") {
                    return std::unexpected(operation_error("invalid_request", "opaque Sequence action is invalid"));
                }
                result.policy.opaque_sequence_decisions.push_back(
                    {value.at("packageIndex").get<std::size_t>(), value.at("nodeId").get<std::string>(),
                     action == "preserve-unchanged" ? axk::PackageOpaqueSequenceAction::preserve_unchanged
                                                    : axk::PackageOpaqueSequenceAction::skip});
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
    case axk::MediaKind::a3k_archive:
        return "a3k-archive";
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

Json program_slot_placements_json(std::span<const axk::PackageProgramSlotPlacement> placements) {
    auto result = Json::array();
    for (const auto &placement : placements) {
        const auto range_json = [](std::span<const axk::PackageProgramSlotRange> ranges) {
            auto values = Json::array();
            for (const auto &range : ranges)
                values.push_back({{"first", range.first}, {"last", range.last}});
            return values;
        };
        auto mappings = Json::array();
        for (const auto &mapping : placement.mappings) {
            mappings.push_back({{"packageIndex", mapping.package_index},
                                {"nodeId", mapping.node_id},
                                {"sourceSlot", mapping.source_slot},
                                {"destinationSlot", mapping.destination_slot},
                                {"requiresUserAction", mapping.requires_user_action}});
        }
        result.push_back(
            {{"placementId", placement.placement_id},
             {"partitionIndex", placement.partition_index},
             {"volumeName", placement.volume_name},
             {"mode", std::string{axk::package_program_slot_placement_mode_name(placement.mode)}},
             {"applied", placement.applied},
             {"suggestedStartSlot", placement.suggested_start_slot ? Json(*placement.suggested_start_slot) : Json{}},
             {"requiredSlotCount", placement.required_slot_count},
             {"availableSlotCount", placement.available_slot_count},
             {"occupiedRanges", range_json(placement.occupied_ranges)},
             {"sourceRanges", range_json(placement.source_ranges)},
             {"destinationRanges", range_json(placement.destination_ranges)},
             {"mappings", std::move(mappings)}});
    }
    return result;
}

Json sfs_index_capacity_json(std::span<const axk::SfsIndexCapacityEstimate> capacities) {
    auto result = Json::array();
    for (const auto &capacity : capacities) {
        auto packages = Json::array();
        for (const auto &usage : capacity.packages) {
            packages.push_back({{"packageIndex", usage.package_index},
                                {"effectiveObjectRecordSlots", usage.effective_object_record_slots},
                                {"volumeScaffoldingRecordSlots", usage.volume_scaffolding_record_slots},
                                {"standaloneRequiredRecordSlots", usage.standalone_required_record_slots},
                                {"plannedObjectRecordSlots", usage.planned_object_record_slots},
                                {"plannedRecordSlots", usage.planned_record_slots},
                                {"reusedObjectCount", usage.reused_object_count},
                                {"allocatedRecordSlots", usage.allocated_record_slots},
                                {"shortfallRecordSlots", usage.shortfall_record_slots}});
        }
        result.push_back({{"partitionIndex", capacity.partition_index},
                          {"indexBlockCount", capacity.index_block_count},
                          {"recordsPerIndexBlock", capacity.records_per_index_block},
                          {"totalRecordSlots", capacity.total_record_slots},
                          {"reservedRecordSlots", capacity.reserved_record_slots},
                          {"allocatableRecordSlots", capacity.allocatable_record_slots},
                          {"usedRecordSlots", capacity.used_record_slots},
                          {"freeRecordSlots", capacity.free_record_slots},
                          {"requiredRecordSlots", capacity.required_record_slots},
                          {"allocatedRecordSlots", capacity.allocated_record_slots},
                          {"shortfallRecordSlots", capacity.shortfall_record_slots},
                          {"remainingRecordSlots", capacity.remaining_record_slots},
                          {"packages", std::move(packages)}});
    }
    return result;
}

Json plan_json(const axk::PackageImportPlan &plan, std::string_view token, std::uint64_t expires_in_seconds) {
    auto warnings = Json::array();
    for (const auto &warning : plan.warnings) {
        warnings.push_back({{"code", warning.code},
                            {"message", warning.message},
                            {"origin", std::string{axk::package_import_warning_origin_name(warning.origin)}},
                            {"packageIndex", warning.package_index ? Json(*warning.package_index) : Json{}},
                            {"nodeId", warning.node_id},
                            {"objectType", warning.object_type},
                            {"objectName", warning.object_name},
                            {"partitionIndex", warning.partition_index ? Json(*warning.partition_index) : Json{}},
                            {"volumeName", warning.volume_name}});
    }
    auto opaque_sequences = Json::array();
    for (const auto &sequence : plan.opaque_sequences) {
        opaque_sequences.push_back(
            {{"packageIndex", sequence.package_index},
             {"nodeId", sequence.node_id},
             {"name", sequence.name},
             {"action", sequence.action ? Json(std::string{axk::package_opaque_sequence_action_name(*sequence.action)})
                                        : Json{}}});
    }
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
            {"opaqueSequences", std::move(opaque_sequences)},
            {"programAssignmentAdjustments", program_assignment_adjustments_json(plan.program_assignment_adjustments)},
            {"programSlotPlacements", program_slot_placements_json(plan.program_slot_placements)},
            {"allocation", std::move(allocation)},
            {"sfsIndexCapacity", sfs_index_capacity_json(plan.sfs_index_capacity)}};
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
    std::set<const VerifiedPackageSet *> counted;
    std::uint64_t total{};
    for (const auto &[token, record] : state.plans) {
        static_cast<void>(token);
        if (record->package_set && counted.emplace(record->package_set.get()).second)
            total += record->package_set->retained_payload_bytes;
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

Json session_import_result(const SessionPackagePlanRecord &record, const axk::app::ImageSessionSummary &summary,
                           bool applied) {
    const auto plan = plan_json(record.plan, record.token, 0U);
    return {{"schemaVersion", plan.at("schemaVersion")},
            {"planId", plan.at("planId")},
            {"targetKind", plan.at("targetKind")},
            {"targetSnapshotId", plan.at("targetSnapshotId")},
            {"actions", plan.at("actions")},
            {"programAssignmentAdjustments", plan.at("programAssignmentAdjustments")},
            {"programSlotPlacements", plan.at("programSlotPlacements")},
            {"allocation", plan.at("allocation")},
            {"imageId", record.image_id},
            {"revision", summary.revision},
            {"objectCount", summary.object_count},
            {"applied", applied}};
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
