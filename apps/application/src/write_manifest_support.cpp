#include "axklib/application/write_operations.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/alteration_transaction.hpp"
#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"
#include <nlohmann/json.hpp>

#include "content_digest.hpp"

#include "write_operations_internal.hpp"

namespace axk::app::write_operations_internal {

using axk::app::detail::file_sha256;
using axk::app::detail::reader_sha256;

void decorate_build_result(Json &result, const WritePlanRecord &record) {
    result["schemaVersion"] = "1.0";
    result["kind"] = write_plan_kind_name(record.kind);
    result["summary"] = record.summary;
}

axk::app::Result<Json> validate_written_image(const std::filesystem::path &path, const axk::app::FileRef &output,
                                              const axk::app::OperationContext &context) {
    auto media = axk::open_media(path, context.cancellation);
    if (!media)
        return std::unexpected(core_error(media.error(), output.relative_path));
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U,
                                                context.cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), output.relative_path));
    auto digest = file_sha256(path, context.cancellation);
    if (!digest)
        return std::unexpected(digest.error());
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected(operation_error("output_read_failed", "could not inspect created image"));
    const auto issues = media->validation_issues();
    return Json{{"output", file_ref_json(output)},
                {"sizeBytes", size},
                {"sha256", *digest},
                {"objectCount", inventory->catalog.objects.size()},
                {"validation", {{"valid", issues.empty()}, {"issueCount", issues.size()}}}};
}

axk::app::Result<axk::BuildManifestKind> parse_build_kind(std::string_view value) {
    if (value == "HDS" || value == "hds")
        return axk::BuildManifestKind::hds;
    if (value == "FLOPPY" || value == "floppy")
        return axk::BuildManifestKind::fat12_floppy;
    if (value == "ISO" || value == "iso")
        return axk::BuildManifestKind::iso9660;
    return std::unexpected(operation_error("invalid_request", "kind must be HDS, FLOPPY, or ISO"));
}

Json operation_report_json(const axk::OperationReport &operation,
                           const std::map<std::string, std::string> &logical_input_paths) {
    auto wire_type = operation.type;
    std::ranges::transform(wire_type, wire_type.begin(), [](char character) {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A')) : character;
    });
    auto removed = Json::array();
    for (const auto id : operation.removed_sfs_ids)
        removed.push_back(id.value);
    auto inserted = Json::array();
    for (const auto id : operation.inserted_sfs_ids)
        inserted.push_back(id.value);
    Json result{{"id", operation.id},
                {"type", std::move(wire_type)},
                {"partitionIndex", operation.partition.value},
                {"volumeName", operation.volume_name},
                {"objectName", operation.object_name},
                {"removedSfsIds", std::move(removed)},
                {"insertedSfsIds", std::move(inserted)},
                {"freedClusters", operation.freed_clusters},
                {"allocatedClusters", operation.allocated_clusters}};
    if (operation.audio_import) {
        const auto source_path = logical_input_paths.find(normalized_path(operation.audio_import->source_path));
        result["audioImport"] = {{"sourcePath", source_path == logical_input_paths.end()
                                                    ? operation.audio_import->source_path.filename().generic_string()
                                                    : source_path->second},
                                 {"sourceFormat", operation.audio_import->source_format},
                                 {"sourceSubtype", operation.audio_import->source_subtype},
                                 {"sourceChannels", operation.audio_import->source_channels},
                                 {"sourceSampleRate", operation.audio_import->source_sample_rate},
                                 {"outputSampleRate", operation.audio_import->output_sample_rate},
                                 {"sourceSampleWidthBits", operation.audio_import->source_sample_width_bits},
                                 {"outputSampleWidthBits", operation.audio_import->output_sample_width_bits},
                                 {"outputFrames", operation.audio_import->output_frames},
                                 {"resampled", operation.audio_import->resampled},
                                 {"quantized", operation.audio_import->quantized},
                                 {"sampleWidthConverted", operation.audio_import->sample_width_converted},
                                 {"ditherAlgorithm", operation.audio_import->dither_algorithm},
                                 {"splitStereo", operation.audio_import->split_stereo},
                                 {"clippedSamples", operation.audio_import->clipped_samples}};
    } else {
        result["audioImport"] = nullptr;
    }
    return result;
}

Json alteration_summary(std::span<const axk::OperationReport> operations) {
    std::uint64_t freed_clusters{};
    std::uint64_t allocated_clusters{};
    for (const auto &operation : operations) {
        freed_clusters += operation.freed_clusters;
        allocated_clusters += operation.allocated_clusters;
    }
    return {{"operationCount", operations.size()},
            {"freedClusters", freed_clusters},
            {"allocatedClusters", allocated_clusters}};
}

Json deletion_inspection_json(const axk::app::ImageObjectDeletionInspection &inspection) {
    Json impacts = Json::array();
    for (const auto &impact : inspection.impacts) {
        impacts.push_back({{"objectId", impact.object_id},
                           {"objectType", impact.object_type},
                           {"objectName", impact.object_name},
                           {"partitionIndex", impact.partition_index ? Json(*impact.partition_index) : Json(nullptr)},
                           {"partitionName", impact.partition_name},
                           {"volumeName", impact.volume_name},
                           {"role", impact.role},
                           {"status", impact.status},
                           {"selected", impact.selected},
                           {"storedSizeBytes", impact.stored_size_bytes},
                           {"freedClusters", impact.freed_clusters},
                           {"prerequisiteObjectIds", impact.prerequisite_object_ids},
                           {"reason", impact.reason}});
    }
    Json references = Json::array();
    for (const auto &reference : inspection.references) {
        references.push_back(
            {{"sourceObjectId", reference.source_object_id},
             {"sourceObjectType", reference.source_object_type},
             {"sourceObjectName", reference.source_object_name},
             {"targetObjectId", reference.target_object_id ? Json(*reference.target_object_id) : Json(nullptr)},
             {"targetObjectType", reference.target_object_type ? Json(*reference.target_object_type) : Json(nullptr)},
             {"targetObjectName", reference.target_object_name ? Json(*reference.target_object_name) : Json(nullptr)},
             {"type", reference.type},
             {"quality", reference.quality},
             {"effect", reference.effect}});
    }
    const auto notices_json = [](const std::vector<axk::app::ImageObjectDeletionNotice> &notices) {
        Json result = Json::array();
        for (const auto &notice : notices) {
            result.push_back({{"code", notice.code}, {"message", notice.message}, {"objectIds", notice.object_ids}});
        }
        return result;
    };
    return {{"canApply", inspection.can_apply},
            {"imageId", inspection.image_id},
            {"revision", inspection.revision},
            {"targetObjectIds", inspection.target_object_ids},
            {"selectedObjectIds", inspection.selected_object_ids},
            {"impacts", std::move(impacts)},
            {"references", std::move(references)},
            {"blockers", notices_json(inspection.blockers)},
            {"warnings", notices_json(inspection.warnings)},
            {"estimatedFreedBytes", inspection.estimated_freed_bytes},
            {"estimatedFreedClusters", inspection.estimated_freed_clusters}};
}

Json wave_data_orphan_inspection_json(const axk::app::ImageWaveDataOrphanInspection &inspection) {
    Json candidates = Json::array();
    for (const auto &candidate : inspection.candidates) {
        candidates.push_back(
            {{"objectId", candidate.object_id},
             {"objectType", candidate.object_type},
             {"objectName", candidate.object_name},
             {"partitionIndex", candidate.partition_index ? Json(*candidate.partition_index) : Json(nullptr)},
             {"partitionName", candidate.partition_name},
             {"volumeName", candidate.volume_name},
             {"storedSizeBytes", candidate.stored_size_bytes},
             {"recoverableBytes", candidate.recoverable_bytes},
             {"recoverableClusters", candidate.recoverable_clusters}});
    }
    return {{"imageId", inspection.image_id},
            {"revision", inspection.revision},
            {"contentScopeId", inspection.content_scope_id},
            {"totalCandidateCount", inspection.total_candidate_count},
            {"candidates", std::move(candidates)}};
}

Json deletion_manifest_json(const axk::AlterationManifest &manifest) {
    Json operations = Json::array();
    for (const auto &operation : manifest.operations) {
        operations.push_back(std::visit(
            [&](const auto &data) -> Json {
                using T = std::decay_t<decltype(data)>;
                if constexpr (std::same_as<T, axk::DeleteSampleBankOperation> ||
                              std::same_as<T, axk::DeleteSampleOperation> ||
                              std::same_as<T, axk::DeleteWaveformOperation> ||
                              std::same_as<T, axk::DeleteProgramOperation>) {
                    const auto *partition = std::get_if<axk::PartitionIndex>(&data.partition);
                    if (partition == nullptr)
                        return Json::object();
                    Json result{{"id", operation.id},
                                {"type", axk::operation_type_name(operation.data)},
                                {"partition_index", partition->value},
                                {"volume_name", data.volume_name}};
                    if constexpr (std::same_as<T, axk::DeleteSampleBankOperation>)
                        result["sample_bank_name"] = data.sample_bank_name;
                    else if constexpr (std::same_as<T, axk::DeleteSampleOperation>)
                        result["sample_name"] = data.sample_name;
                    else if constexpr (std::same_as<T, axk::DeleteProgramOperation>)
                        result["program_number"] = data.program_number;
                    else
                        result["waveform_name"] = data.waveform_name;
                    return result;
                } else {
                    return Json::object();
                }
            },
            operation.data));
    }
    return {{"schema_version", manifest.schema_version}, {"operations", std::move(operations)}};
}

axk::Result<axk::app::PreparedLocalBuildManifest>
prepare_local_manifest_document(const std::filesystem::path &manifest_path,
                                std::span<const std::filesystem::path> paths) {
    std::ifstream input{manifest_path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io, "could not open manifest"));
    }
    Json document;
    try {
        input >> document;
    } catch (const Json::exception &) {
        return std::unexpected(axk::make_error(axk::ErrorCode::manifest_invalid, axk::ErrorCategory::manifest,
                                               "manifest is not valid JSON"));
    }

    std::map<std::string, axk::app::LocalManifestInputBinding> bindings;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        bindings.try_emplace(normalized_path(paths[index]),
                             axk::app::LocalManifestInputBinding{std::format("inputs/{:04}", index), paths[index]});
    }
    std::set<std::string> replaced;
    const auto rewrite = [&](const auto &self, Json &value) -> void {
        if (value.is_string()) {
            auto candidate = std::filesystem::path{value.get_ref<const std::string &>()};
            if (candidate.is_relative())
                candidate = manifest_path.parent_path() / candidate;
            const auto found = bindings.find(normalized_path(candidate));
            if (found != bindings.end()) {
                value = found->second.manifest_path;
                replaced.insert(found->first);
            }
            return;
        }
        if (value.is_array() || value.is_object()) {
            for (auto &item : value)
                self(self, item);
        }
    };
    rewrite(rewrite, document);
    if (replaced.size() != bindings.size()) {
        return std::unexpected(axk::make_error(axk::ErrorCode::manifest_invalid, axk::ErrorCategory::manifest,
                                               "could not bind every manifest input path"));
    }
    axk::app::PreparedLocalBuildManifest result{std::move(document), {}};
    for (auto &[canonical, binding] : bindings) {
        static_cast<void>(canonical);
        result.bindings.push_back(std::move(binding));
    }
    return result;
}

} // namespace axk::app::write_operations_internal
