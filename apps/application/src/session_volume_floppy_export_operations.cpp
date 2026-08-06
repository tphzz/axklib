#include "axklib/application/session_volume_floppy_export_operations.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"

namespace {

using Json = nlohmann::json;

struct ExportVolume {
    std::string content_id;
    std::uint32_t directory_id{};
    std::string name;
    std::string display_name;
    std::string directory_name;
};

struct ExportContext {
    std::string image_id;
    std::uint64_t revision{};
    std::string source_path;
    axk::app::ImageContentItem scope;
    axk::VolumeFloppyExportPlanSummary plan;
    std::string default_directory_name;
    std::vector<ExportVolume> volumes;
};

class TemporaryDirectoryCleanup {
  public:
    explicit TemporaryDirectoryCleanup(std::filesystem::path path) : path_{std::move(path)} {}
    ~TemporaryDirectoryCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

  private:
    std::filesystem::path path_;
};

axk::app::Error operation_error(std::string code, std::string message) {
    return {std::move(code), std::move(message), {}};
}

axk::app::Error core_error(const axk::Error &error) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "volume_floppy_export_failed",
            error.message, std::move(context)};
}

std::string safe_name(std::string_view value, std::string_view fallback) {
    std::string result;
    result.reserve(value.size());
    bool prior_space{};
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            if (!result.empty() && !prior_space)
                result.push_back(' ');
            prior_space = true;
            continue;
        }
        prior_space = false;
        if (byte < 0x20U || character == '<' || character == '>' || character == ':' || character == '"' ||
            character == '/' || character == '\\' || character == '|' || character == '?' || character == '*') {
            result.push_back('_');
        } else {
            result.push_back(character);
        }
    }
    while (!result.empty() && (result.front() == ' ' || result.front() == '.'))
        result.erase(result.begin());
    while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
        result.pop_back();
    if (result.empty())
        result = fallback;
    std::string folded;
    folded.reserve(result.size());
    for (const auto character : result)
        folded.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    static const std::set<std::string, std::less<>> reserved{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (reserved.contains(folded))
        result.insert(result.begin(), '_');
    return result;
}

std::string fold_name(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

void assign_directory_names(std::vector<ExportVolume> &volumes) {
    std::map<std::string, std::size_t, std::less<>> counts;
    for (const auto &volume : volumes)
        ++counts[fold_name(safe_name(volume.name, "Volume"))];
    std::set<std::string, std::less<>> used;
    for (auto &volume : volumes) {
        const auto base = safe_name(volume.name, "Volume");
        const auto stem =
            counts.at(fold_name(base)) > 1U ? base + " [volume-" + std::to_string(volume.directory_id) + "]" : base;
        auto name = stem;
        for (std::size_t suffix = 2U; !used.emplace(fold_name(name)).second; ++suffix)
            name = stem + " (" + std::to_string(suffix) + ")";
        volume.directory_name = std::move(name);
    }
}

axk::app::Result<std::pair<std::string, std::uint64_t>> parse_identity(const Json &input) {
    try {
        const auto image_id = input.at("imageId").get<std::string>();
        const auto revision = input.at("expectedRevision").get<std::uint64_t>();
        if (image_id.empty() || revision == 0U)
            return std::unexpected(operation_error("invalid_request", "imageId and expectedRevision are required"));
        return std::pair{image_id, revision};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "imageId and expectedRevision are required"));
    }
}

axk::app::Result<std::string> parse_scope_id(const Json &input) {
    try {
        const auto scope_id = input.at("scopeId").get<std::string>();
        if (scope_id.empty())
            return std::unexpected(operation_error("invalid_request", "scopeId is required"));
        return scope_id;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "scopeId is required"));
    }
}

std::string unit_name(axk::MediaConversionIssueUnit unit) {
    switch (unit) {
    case axk::MediaConversionIssueUnit::bytes:
        return "BYTES";
    case axk::MediaConversionIssueUnit::directory_entries:
        return "DIRECTORY_ENTRIES";
    case axk::MediaConversionIssueUnit::floppy_images:
        return "FLOPPY_IMAGES";
    }
    return "BYTES";
}

Json issue_json(const axk::MediaConversionIssue &issue) {
    Json result{{"code", issue.code}, {"message", issue.message}, {"blocking", issue.blocking}};
    if (issue.measurement) {
        result["measurement"] = {{"required", issue.measurement->required},
                                 {"available", issue.measurement->available},
                                 {"unit", unit_name(issue.measurement->unit)}};
    } else {
        result["measurement"] = nullptr;
    }
    return result;
}

axk::app::Result<ExportContext> resolve_context(const axk::app::ImageContentScope &content,
                                                const axk::app::ImageSessionRead &session,
                                                const axk::MediaBuildLimits &limits,
                                                const axk::CancellationToken &cancellation) {
    if (content.item.kind != "partition" || !content.item.partition_index) {
        return std::unexpected(
            operation_error("content_scope_invalid", "volume floppy export requires an exact partition scope"));
    }
    if (session.media->kind() != axk::MediaKind::sfs || session.source.kind != axk::app::ImageSourceKind::file) {
        return std::unexpected(operation_error("volume_floppy_export_unsupported",
                                               "volume floppy export requires a file-backed SFS image session"));
    }
    axk::VolumeFloppyExportRequest request{*content.item.partition_index};
    auto plan =
        axk::plan_volume_floppy_export(session.reader, session.source.relative_path, request, limits, cancellation);
    if (!plan)
        return std::unexpected(core_error(plan.error()));

    ExportContext result{session.image_id,
                         session.revision,
                         session.source.relative_path,
                         content.item,
                         std::move(*plan),
                         safe_name(content.item.display_name, "Partition") + " floppies",
                         {}};
    for (const auto &summary : result.plan.volumes) {
        const auto item = std::ranges::find_if(content.children, [&](const axk::app::ImageContentItem &candidate) {
            return candidate.kind == "volume" && candidate.volume_directory_id == summary.directory_id;
        });
        if (item == content.children.end()) {
            return std::unexpected(
                operation_error("image_session_invalid", "volume content is missing its exact storage identity"));
        }
        result.volumes.push_back({item->id, summary.directory_id, summary.name, item->display_name, {}});
    }
    assign_directory_names(result.volumes);
    return result;
}

Json inspection_json(const ExportContext &context) {
    Json volumes = Json::array();
    std::size_t ready_count{};
    std::size_t empty_count{};
    std::size_t blocked_count{};
    std::size_t disk_count{};
    std::uint64_t disk_bytes{};
    for (std::size_t index = 0U; index < context.plan.volumes.size(); ++index) {
        const auto &summary = context.plan.volumes[index];
        const auto &volume = context.volumes[index];
        const auto state = summary.object_count == 0U ? "EMPTY" : summary.can_export ? "READY" : "BLOCKED";
        ready_count += state == std::string_view{"READY"} ? 1U : 0U;
        empty_count += state == std::string_view{"EMPTY"} ? 1U : 0U;
        blocked_count += state == std::string_view{"BLOCKED"} ? 1U : 0U;
        if (summary.can_export && summary.object_count != 0U) {
            disk_count += summary.floppy_image_count;
            disk_bytes += summary.projected_disk_bytes;
        }
        Json issues = Json::array();
        for (const auto &issue : summary.issues)
            issues.push_back(issue_json(issue));
        volumes.push_back({{"contentId", volume.content_id},
                           {"name", volume.name},
                           {"displayName", volume.display_name},
                           {"partitionIndex", context.plan.partition_index},
                           {"volumeDirectoryId", volume.directory_id},
                           {"objectCount", summary.object_count},
                           {"payloadBytes", summary.payload_bytes},
                           {"state", state},
                           {"directoryName", state == std::string_view{"READY"} ? Json(volume.directory_name) : Json{}},
                           {"floppyImageCount", summary.floppy_image_count},
                           {"projectedDiskBytes", summary.projected_disk_bytes},
                           {"issues", std::move(issues)}});
    }
    return {{"imageId", context.image_id},
            {"revision", context.revision},
            {"sourceMediaKind", "SFS"},
            {"scopeId", context.scope.id},
            {"scopeName", context.scope.display_name},
            {"defaultDirectoryName", context.default_directory_name},
            {"volumeCount", context.plan.volumes.size()},
            {"exportableCount", ready_count},
            {"emptyCount", empty_count},
            {"blockedCount", blocked_count},
            {"totalFloppyImageCount", disk_count},
            {"projectedDiskBytes", disk_bytes},
            {"volumes", std::move(volumes)}};
}

axk::app::Result<void> write_text(const std::filesystem::path &path, std::string_view value) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(operation_error("volume_floppy_export_failed", "could not create the export report"));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.flush();
    if (!output)
        return std::unexpected(operation_error("volume_floppy_export_failed", "could not write the export report"));
    return {};
}

Json download_json(const axk::app::DownloadArchiveSnapshot &retained) {
    return {{"archiveId", retained.reference.archive_id},
            {"filename", retained.filename},
            {"sizeBytes", retained.size_bytes},
            {"expiresInSeconds", retained.expires_in_seconds},
            {"contentPath", "/api/v1/download-archives/" + retained.reference.archive_id + "/content"}};
}

void report_progress(const axk::app::OperationContext &context, axk::ProgressPhase phase, std::size_t completed,
                     std::size_t total, std::string label) {
    if (context.progress)
        context.progress->report({phase, completed, total, std::move(label), std::nullopt});
}

std::string status_name(axk::VolumeFloppyExportStatus status) {
    switch (status) {
    case axk::VolumeFloppyExportStatus::exported:
        return "EXPORTED";
    case axk::VolumeFloppyExportStatus::skipped_empty:
        return "SKIPPED_EMPTY";
    case axk::VolumeFloppyExportStatus::blocked:
        return "BLOCKED";
    case axk::VolumeFloppyExportStatus::failed:
        return "FAILED";
    }
    return "FAILED";
}

} // namespace

axk::app::Result<void>
axk::app::bind_session_volume_floppy_export_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                       ImageSessionManager &images, DownloadArchiveStore &downloads,
                                                       const axk::MediaBuildLimits &media_limits) {
    if (!registry.is_implemented("images.volume_floppy_export.inspect")) {
        auto bound =
            registry.bind("images.volume_floppy_export.inspect",
                          [&images, media_limits](const Json &input, const OperationContext &context) -> Result<Json> {
                              auto identity = parse_identity(input);
                              if (!identity)
                                  return std::unexpected(identity.error());
                              auto scope_id = parse_scope_id(input);
                              if (!scope_id)
                                  return std::unexpected(scope_id.error());
                              auto content = images.content_scope(identity->first, context.owner_id, *scope_id);
                              if (!content)
                                  return std::unexpected(content.error());
                              auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                              if (!session)
                                  return std::unexpected(session.error());
                              auto resolved = resolve_context(*content, *session, media_limits, context.cancellation);
                              if (!resolved)
                                  return std::unexpected(resolved.error());
                              return inspection_json(*resolved);
                          });
        if (!bound)
            return bound;
    }
    if (registry.is_implemented("images.volume_floppy_export"))
        return {};
    return registry.bind(
        "images.volume_floppy_export",
        [&sandbox, &images, &downloads, media_limits](const Json &input,
                                                      const OperationContext &context) -> Result<Json> {
            auto identity = parse_identity(input);
            if (!identity)
                return std::unexpected(identity.error());
            auto scope_id = parse_scope_id(input);
            if (!scope_id)
                return std::unexpected(scope_id.error());
            auto content = images.content_scope(identity->first, context.owner_id, *scope_id);
            if (!content)
                return std::unexpected(content.error());
            auto session = images.begin_read(identity->first, context.owner_id, identity->second);
            if (!session)
                return std::unexpected(session.error());
            auto resolved = resolve_context(*content, *session, media_limits, context.cancellation);
            if (!resolved)
                return std::unexpected(resolved.error());
            auto context_data = std::move(*resolved);

            std::vector<axk::VolumeFloppyExportTarget> targets;
            for (std::size_t index = 0U; index < context_data.plan.volumes.size(); ++index) {
                const auto &summary = context_data.plan.volumes[index];
                if (summary.object_count != 0U && summary.can_export)
                    targets.push_back({summary.directory_id, context_data.volumes[index].directory_name});
            }
            if (targets.empty()) {
                return std::unexpected(
                    operation_error("volume_floppy_export_empty", "the selected partition has no exportable volumes"));
            }

            auto staging = sandbox.create_staging_directory("axklib-volume-floppies");
            if (!staging)
                return std::unexpected(staging.error());
            TemporaryDirectoryCleanup cleanup{*staging};
            report_progress(context, axk::ProgressPhase::resolving, 0U, targets.size() + 1U,
                            "Planning volume floppy sets");
            const axk::VolumeFloppyExportRequest request{context_data.plan.partition_index};
            auto batch = axk::write_volume_floppy_export(session->reader, session->source.relative_path, request,
                                                         *staging, targets, media_limits, context.cancellation);
            if (!batch)
                return std::unexpected(core_error(batch.error()));

            Json entries = Json::array();
            std::size_t exported_count{};
            std::size_t skipped_count{};
            std::size_t blocked_count{};
            std::size_t failed_count{};
            std::size_t disk_count{};
            std::uint64_t disk_bytes{};
            for (std::size_t index = 0U; index < batch->volumes.size(); ++index) {
                const auto &written = batch->volumes[index];
                const auto &volume = context_data.volumes[index];
                const auto status = status_name(written.status);
                exported_count += written.status == axk::VolumeFloppyExportStatus::exported ? 1U : 0U;
                skipped_count += written.status == axk::VolumeFloppyExportStatus::skipped_empty ? 1U : 0U;
                blocked_count += written.status == axk::VolumeFloppyExportStatus::blocked ? 1U : 0U;
                failed_count += written.status == axk::VolumeFloppyExportStatus::failed ? 1U : 0U;
                disk_count += written.disks.size();
                disk_bytes += written.size_bytes;
                Json disks = Json::array();
                for (const auto &disk : written.disks) {
                    disks.push_back({{"index", disk.index},
                                     {"path", volume.directory_name + "/" + disk.path.filename().string()},
                                     {"sizeBytes", disk.size_bytes},
                                     {"sha256", disk.sha256}});
                }
                Json error;
                if (written.error)
                    error = {{"code", std::to_string(static_cast<std::uint32_t>(written.error->code))},
                             {"message", written.error->message}};
                entries.push_back(
                    {{"contentId", volume.content_id},
                     {"name", volume.name},
                     {"displayName", volume.display_name},
                     {"partitionIndex", context_data.plan.partition_index},
                     {"volumeDirectoryId", volume.directory_id},
                     {"status", status},
                     {"directoryPath",
                      written.status == axk::VolumeFloppyExportStatus::exported ? Json(volume.directory_name) : Json{}},
                     {"sizeBytes", written.size_bytes},
                     {"disks", std::move(disks)},
                     {"error", std::move(error)}});
            }
            if (exported_count == 0U) {
                return std::unexpected(
                    operation_error("volume_floppy_export_empty", "none of the partition volumes could be exported"));
            }

            Json report{{"schemaVersion", "1.0"},
                        {"imageId", context_data.image_id},
                        {"revision", context_data.revision},
                        {"source", {{"kind", "SFS"}, {"relativePath", context_data.source_path}}},
                        {"scope", {{"contentId", context_data.scope.id}, {"name", context_data.scope.display_name}}},
                        {"summary",
                         {{"volumeCount", batch->volumes.size()},
                          {"exportedCount", exported_count},
                          {"skippedCount", skipped_count},
                          {"blockedCount", blocked_count},
                          {"failedCount", failed_count},
                          {"floppyImageCount", disk_count},
                          {"diskBytes", disk_bytes}}},
                        {"volumes", entries}};
            const auto report_path = std::string{"volume-floppies.axklib.json"};
            if (auto written = write_text(*staging / report_path, report.dump(2) + '\n'); !written)
                return std::unexpected(written.error());
            session->lease.reset();

            Json destination;
            try {
                destination = input.at("destination");
            } catch (const Json::exception &) {
                return std::unexpected(operation_error("invalid_request", "destination is required"));
            }
            Json result{{"imageId", context_data.image_id}, {"revision", context_data.revision},
                        {"scopeId", context_data.scope.id}, {"volumeCount", batch->volumes.size()},
                        {"exportedCount", exported_count},  {"skippedCount", skipped_count},
                        {"blockedCount", blocked_count},    {"failedCount", failed_count},
                        {"floppyImageCount", disk_count},   {"diskBytes", disk_bytes},
                        {"reportPath", report_path},        {"volumes", std::move(entries)}};
            report_progress(context, axk::ProgressPhase::publishing, targets.size(), targets.size() + 1U,
                            "Publishing volume floppy sets");
            const auto kind = destination.value("kind", std::string{});
            if (kind == "WORKSPACE") {
                DirectoryRef output;
                try {
                    const auto &value = destination.at("output");
                    output = {value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
                } catch (const Json::exception &) {
                    return std::unexpected(
                        operation_error("invalid_request", "workspace destination requires an output directory"));
                }
                if (auto published = sandbox.publish_directory(output, false, *staging); !published)
                    return std::unexpected(published.error());
                result["destination"] = "WORKSPACE";
                result["output"] = {{"rootId", output.root_id}, {"relativePath", output.relative_path}};
                result["download"] = nullptr;
            } else if (kind == "DOWNLOAD") {
                const auto directory_name = destination.value("directoryName", std::string{});
                if (directory_name.empty() || directory_name == "." || directory_name == ".." ||
                    directory_name.find_first_of("/\\") != std::string::npos) {
                    return std::unexpected(operation_error("invalid_request", "download directory name is invalid"));
                }
                auto retained = downloads.create_owned_directory(context.owner_id, *staging, directory_name + ".tar");
                if (!retained)
                    return std::unexpected(retained.error());
                result["destination"] = "DOWNLOAD";
                result["output"] = nullptr;
                result["download"] = download_json(*retained);
            } else {
                return std::unexpected(
                    operation_error("invalid_request", "destination kind must be WORKSPACE or DOWNLOAD"));
            }
            report_progress(context, axk::ProgressPhase::publishing, targets.size() + 1U, targets.size() + 1U,
                            "Volume floppy sets ready");
            return result;
        });
}
