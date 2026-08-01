#include "axklib/application/session_media_conversion_operations.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/utf8.hpp"

namespace {

using Json = nlohmann::json;

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

axk::app::Error operation_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

axk::app::Error core_error(const axk::Error &error) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    return {"media_conversion_failed", error.message, std::move(context)};
}

axk::app::Result<std::pair<std::string, std::uint64_t>> parse_session_identity(const Json &input) {
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

std::string safe_stem(std::string_view input, std::string_view fallback) {
    std::string result;
    result.reserve(input.size());
    for (const auto character : input) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(byte) != 0 || character == '-' || character == '_' ? character : '_');
    }
    result.erase(
        std::unique(result.begin(), result.end(), [](char left, char right) { return left == '_' && right == '_'; }),
        result.end());
    while (!result.empty() && result.front() == '_')
        result.erase(result.begin());
    while (!result.empty() && result.back() == '_')
        result.pop_back();
    return result.empty() ? std::string{fallback} : result;
}

std::string source_stem(const axk::app::ImageSourceRef &source) {
    const auto path = axk::text::path_from_utf8(source.relative_path);
    if (!path)
        return "Sampler";
    return safe_stem(axk::text::path_to_utf8(path->stem()), "Sampler");
}

axk::app::Result<axk::MediaConversionRequest> parse_conversion_request(const Json &input,
                                                                       std::string_view default_iso_id) {
    try {
        axk::MediaConversionRequest request;
        request.partition_index = input.at("partitionIndex").get<std::uint32_t>();
        const auto format = input.at("format").get<std::string>();
        if (format == "ISO9660") {
            request.format = axk::MediaImageFormat::iso9660;
            request.scope = axk::MediaConversionScope::partition;
            request.iso_volume_id = input.value("isoVolumeId", std::string{default_iso_id});
        } else if (format == "FAT12_FLOPPY") {
            request.format = axk::MediaImageFormat::fat12_floppy;
            request.scope = axk::MediaConversionScope::volume;
            request.volume_directory_id = input.at("volumeDirectoryId").get<std::uint32_t>();
        } else {
            return std::unexpected(operation_error("invalid_request", "format must be ISO9660 or FAT12_FLOPPY"));
        }
        return request;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error(
            "invalid_request", "format, partitionIndex, and the volume identity for floppy exports are required"));
    }
}

std::string format_name(axk::MediaImageFormat format) {
    return format == axk::MediaImageFormat::iso9660 ? "ISO9660" : "FAT12_FLOPPY";
}

std::string scope_name(axk::MediaConversionScope scope) {
    return scope == axk::MediaConversionScope::partition ? "PARTITION" : "VOLUME";
}

std::string extension(axk::MediaImageFormat format) {
    return format == axk::MediaImageFormat::iso9660 ? ".iso" : ".ima";
}

std::string media_type(axk::MediaImageFormat format) {
    return format == axk::MediaImageFormat::iso9660 ? "application/x-iso9660-image" : "application/octet-stream";
}

std::string default_filename(std::string_view source, const axk::MediaConversionPlanSummary &summary) {
    const auto scope = summary.scope == axk::MediaConversionScope::partition || summary.volumes.empty()
                           ? summary.partition_name
                           : summary.volumes.front().name;
    return std::format("{}_p{:02}_{}{}", safe_stem(source, "Sampler"), summary.partition_index,
                       safe_stem(scope, summary.scope == axk::MediaConversionScope::partition ? "Partition" : "Volume"),
                       extension(summary.format));
}

Json issue_json(const axk::MediaConversionIssue &issue) {
    return {{"code", issue.code},
            {"message", issue.message},
            {"blocking", issue.blocking},
            {"requiredBytes", issue.required_bytes ? Json(*issue.required_bytes) : Json{}},
            {"availableBytes", issue.available_bytes ? Json(*issue.available_bytes) : Json{}}};
}

Json plan_json(std::string_view image_id, std::uint64_t revision, std::string_view source,
               const axk::MediaConversionPlanSummary &summary) {
    Json volumes = Json::array();
    for (const auto &volume : summary.volumes) {
        volumes.push_back({{"volumeDirectoryId", volume.directory_id},
                           {"name", volume.name},
                           {"objectCount", volume.object_count},
                           {"payloadBytes", volume.payload_bytes}});
    }
    Json issues = Json::array();
    for (const auto &issue : summary.issues)
        issues.push_back(issue_json(issue));
    return {{"imageId", image_id},
            {"revision", revision},
            {"format", format_name(summary.format)},
            {"scope", scope_name(summary.scope)},
            {"partitionIndex", summary.partition_index},
            {"partitionName", summary.partition_name},
            {"canExport", summary.can_export},
            {"objectCount", summary.object_count},
            {"payloadBytes", summary.payload_bytes},
            {"projectedOutputBytes", summary.projected_output_bytes},
            {"capacityBytes", summary.capacity_bytes},
            {"volumes", std::move(volumes)},
            {"issues", std::move(issues)},
            {"defaultFilename", default_filename(source, summary)}};
}

axk::app::Result<axk::app::FileRef> parse_output(const Json &destination) {
    try {
        const auto &output = destination.at("output");
        axk::app::FileRef result{output.at("rootId").get<std::string>(), output.at("relativePath").get<std::string>()};
        if (result.root_id.empty() || result.relative_path.empty())
            return std::unexpected(operation_error("invalid_request", "workspace destination requires an output file"));
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "workspace destination requires an output file"));
    }
}

axk::app::Result<std::string> resolved_filename(std::string filename, std::string_view required_extension) {
    if (filename.empty() || filename == "." || filename == ".." || filename.find_first_of("/\\") != std::string::npos)
        return std::unexpected(operation_error("invalid_request", "download filename is invalid"));
    const auto path = axk::text::path_from_utf8(filename);
    if (!path)
        return std::unexpected(operation_error("invalid_request", "output filename is not valid UTF-8"));
    if (path->extension().empty())
        return filename + std::string{required_extension};
    if (path->extension() != required_extension)
        return std::unexpected(operation_error("invalid_request", "output filename has an unsupported extension"));
    return filename;
}

void report_progress(const axk::app::OperationContext &context, axk::ProgressPhase phase, std::size_t completed,
                     std::size_t total, std::string label) {
    if (context.progress)
        context.progress->report({phase, completed, total, std::move(label), std::nullopt});
}

} // namespace

axk::app::Result<void> axk::app::bind_session_media_conversion_operations(OperationRegistry &registry,
                                                                          const Sandbox &sandbox,
                                                                          ImageSessionManager &images,
                                                                          DownloadArchiveStore &downloads,
                                                                          const axk::MediaBuildLimits &media_limits) {
    if (!registry.is_implemented("images.media_conversion.inspect")) {
        auto bound =
            registry.bind("images.media_conversion.inspect",
                          [&images, media_limits](const Json &input, const OperationContext &context) -> Result<Json> {
                              const auto identity = parse_session_identity(input);
                              if (!identity)
                                  return std::unexpected(identity.error());
                              auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                              if (!session)
                                  return std::unexpected(session.error());
                              if (session->source.kind != ImageSourceKind::file)
                                  return std::unexpected(operation_error(
                                      "media_conversion_unsupported", "Only HDA and HDS image files can be converted"));
                              const auto source = source_stem(session->source);
                              const auto request = parse_conversion_request(input, source);
                              if (!request)
                                  return std::unexpected(request.error());
                              auto plan = axk::plan_media_conversion(session->reader, session->source.relative_path,
                                                                     *request, media_limits, context.cancellation);
                              if (!plan)
                                  return std::unexpected(core_error(plan.error()));
                              return plan_json(identity->first, identity->second, source, *plan);
                          });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("images.media_conversion")) {
        auto bound = registry.bind(
            "images.media_conversion",
            [&sandbox, &images, &downloads, media_limits](const Json &input,
                                                          const OperationContext &context) -> Result<Json> {
                const auto identity = parse_session_identity(input);
                if (!identity)
                    return std::unexpected(identity.error());
                auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                if (!session)
                    return std::unexpected(session.error());
                if (session->source.kind != ImageSourceKind::file)
                    return std::unexpected(operation_error("media_conversion_unsupported",
                                                           "Only HDA and HDS image files can be converted"));
                const auto source = source_stem(session->source);
                const auto request = parse_conversion_request(input, source);
                if (!request)
                    return std::unexpected(request.error());
                report_progress(context, axk::ProgressPhase::resolving, 0U, 3U, "Checking media conversion");
                auto plan = axk::plan_media_conversion(session->reader, session->source.relative_path, *request,
                                                       media_limits, context.cancellation);
                if (!plan)
                    return std::unexpected(core_error(plan.error()));
                if (!plan->can_export)
                    return std::unexpected(operation_error("media_conversion_blocked", plan->issues.front().message));

                auto staging = sandbox.create_staging_directory("axklib-media-conversion");
                if (!staging)
                    return std::unexpected(staging.error());
                TemporaryDirectoryCleanup cleanup{*staging};
                const auto staged_file = *staging / ("converted" + extension(request->format));
                report_progress(context, axk::ProgressPhase::writing, 1U, 3U, "Creating sampler media image");
                auto written = axk::write_media_conversion(session->reader, session->source.relative_path, *request,
                                                           staged_file, false, media_limits, context.cancellation);
                if (!written)
                    return std::unexpected(core_error(written.error()));
                session->lease.reset();

                Json destination;
                try {
                    destination = input.at("destination");
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error("invalid_request", "destination is required"));
                }
                auto result = plan_json(identity->first, identity->second, source, *plan);
                result["sizeBytes"] = written->size_bytes;
                report_progress(context, axk::ProgressPhase::publishing, 2U, 3U, "Publishing sampler media image");
                const auto kind = destination.value("kind", std::string{});
                if (kind == "WORKSPACE") {
                    auto output = parse_output(destination);
                    if (!output)
                        return std::unexpected(output.error());
                    const auto output_path = axk::text::path_from_utf8(output->relative_path);
                    if (!output_path)
                        return std::unexpected(operation_error("invalid_request", "output path is not valid UTF-8"));
                    if (!output_path->extension().empty() && output_path->extension() != extension(request->format))
                        return std::unexpected(
                            operation_error("invalid_request", "output filename has an unsupported extension"));
                    if (output_path->extension().empty())
                        output->relative_path += extension(request->format);
                    auto reader = axk::FileReader::open(staged_file);
                    if (!reader)
                        return std::unexpected(core_error(reader.error()));
                    if (auto published = sandbox.publish_file(*output, destination.value("overwrite", false), **reader);
                        !published) {
                        return std::unexpected(published.error());
                    }
                    result["destination"] = "WORKSPACE";
                    result["output"] = {{"rootId", output->root_id}, {"relativePath", output->relative_path}};
                    result["download"] = nullptr;
                } else if (kind == "DOWNLOAD") {
                    auto filename = resolved_filename(destination.value("filename", result.at("defaultFilename")),
                                                      extension(request->format));
                    if (!filename)
                        return std::unexpected(filename.error());
                    auto retained = downloads.retain_owned_file(context.owner_id, staged_file, *filename,
                                                                media_type(request->format), context.cancellation,
                                                                context.progress);
                    if (!retained)
                        return std::unexpected(retained.error());
                    result["destination"] = "DOWNLOAD";
                    result["output"] = nullptr;
                    result["download"] = {
                        {"archiveId", retained->reference.archive_id},
                        {"filename", retained->filename},
                        {"sizeBytes", retained->size_bytes},
                        {"expiresInSeconds", retained->expires_in_seconds},
                        {"contentPath", "/api/v1/download-archives/" + retained->reference.archive_id + "/content"}};
                } else {
                    return std::unexpected(
                        operation_error("invalid_request", "destination kind must be WORKSPACE or DOWNLOAD"));
                }
                report_progress(context, axk::ProgressPhase::publishing, 3U, 3U, "Sampler media image ready");
                return result;
            });
        if (!bound)
            return bound;
    }
    return {};
}
