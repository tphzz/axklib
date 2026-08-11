#include "axklib/application/session_volume_package_operations.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
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
#include "axklib/package.hpp"

namespace {

using Json = nlohmann::json;

struct VolumeCandidate {
    std::string content_id;
    std::string name;
    std::string display_name;
    std::uint8_t partition_index{};
    std::uint32_t volume_directory_id{};
    std::string container_directory;
    std::size_t object_count{};
    std::string package_path;
};

struct VolumeExportContext {
    std::string image_id;
    std::uint64_t revision{};
    std::string source_kind;
    std::string source_path;
    axk::app::ImageContentItem scope;
    std::string default_directory_name;
    std::vector<VolumeCandidate> volumes;
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

axk::app::Error operation_error(std::string code, std::string message,
                                std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context)};
}

axk::app::Error core_error(const axk::Error &error) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "volume_package_export_failed",
            error.message, std::move(context)};
}

std::string media_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "SFS";
    case axk::MediaKind::iso9660:
        return "ISO9660";
    case axk::MediaKind::a3k_archive:
    case axk::MediaKind::fat12_floppy:
    case axk::MediaKind::fat12_floppy_set:
    case axk::MediaKind::standalone_object:
    case axk::MediaKind::axk_object_directory:
        return "UNSUPPORTED";
    }
    return "UNSUPPORTED";
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

std::string technical_suffix(const VolumeCandidate &volume) {
    const auto directory = std::filesystem::path{volume.container_directory}.filename().string();
    if (!directory.empty() && directory != ".")
        return safe_name(directory, "volume");
    return std::format("volume-{}", volume.volume_directory_id);
}

void assign_package_paths(std::vector<VolumeCandidate> &volumes) {
    std::map<std::string, std::size_t, std::less<>> base_counts;
    for (const auto &volume : volumes)
        ++base_counts[fold_name(safe_name(volume.name, "Volume"))];
    std::set<std::string, std::less<>> used;
    for (auto &volume : volumes) {
        const auto base = safe_name(volume.name, "Volume");
        auto stem =
            base_counts.at(fold_name(base)) > 1U ? std::format("{} [{}]", base, technical_suffix(volume)) : base;
        auto path = stem + ".axkvol";
        for (std::size_t suffix = 2U; !used.emplace(fold_name(path)).second; ++suffix)
            path = std::format("{} ({}){}", stem, suffix, ".axkvol");
        volume.package_path = std::move(path);
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

axk::app::Result<VolumeExportContext> resolve_context(const axk::app::ImageContentScope &content,
                                                      const axk::app::ImageSessionRead &session) {
    if (content.item.kind != "partition")
        return std::unexpected(
            operation_error("content_scope_invalid", "volume package export requires a partition or CD-ROM group"));
    if (session.media->kind() != axk::MediaKind::sfs && session.media->kind() != axk::MediaKind::iso9660) {
        return std::unexpected(operation_error("volume_package_export_unsupported",
                                               "volume package export requires an SFS or CD-ROM image session"));
    }

    VolumeExportContext result{session.image_id,
                               session.revision,
                               media_kind_name(session.media->kind()),
                               session.source.relative_path,
                               content.item,
                               safe_name(content.item.display_name, "Volume") + " packages",
                               {}};
    for (const auto &item : content.children) {
        if (item.kind != "volume")
            continue;
        if (!item.partition_index || !item.volume_directory_id) {
            return std::unexpected(
                operation_error("image_session_invalid", "volume content is missing its exact storage identity"));
        }
        VolumeCandidate volume{
            item.id, item.name, item.display_name, *item.partition_index, *item.volume_directory_id, {}, 0U, {}};
        for (const auto *object : session.catalog_objects) {
            if (!object->placement || object->placement->partition.value != volume.partition_index ||
                object->placement->volume_directory.value != volume.volume_directory_id) {
                continue;
            }
            ++volume.object_count;
            if (volume.container_directory.empty())
                volume.container_directory = object->placement->container_directory;
        }
        result.volumes.push_back(std::move(volume));
    }
    if (result.volumes.empty())
        return std::unexpected(operation_error("volume_package_export_empty", "the selected scope has no volumes"));
    assign_package_paths(result.volumes);
    return result;
}

Json inspection_json(const VolumeExportContext &context) {
    Json volumes = Json::array();
    std::size_t exportable_count{};
    for (const auto &volume : context.volumes) {
        const auto ready = volume.object_count != 0U;
        exportable_count += ready ? 1U : 0U;
        volumes.push_back({{"contentId", volume.content_id},
                           {"name", volume.name},
                           {"displayName", volume.display_name},
                           {"partitionIndex", volume.partition_index},
                           {"volumeDirectoryId", volume.volume_directory_id},
                           {"objectCount", volume.object_count},
                           {"state", ready ? "READY" : "EMPTY"},
                           {"packagePath", ready ? Json(volume.package_path) : Json{}}});
    }
    return {{"imageId", context.image_id},
            {"revision", context.revision},
            {"sourceMediaKind", context.source_kind},
            {"scopeId", context.scope.id},
            {"scopeName", context.scope.display_name},
            {"defaultDirectoryName", context.default_directory_name},
            {"volumeCount", context.volumes.size()},
            {"exportableCount", exportable_count},
            {"emptyCount", context.volumes.size() - exportable_count},
            {"volumes", std::move(volumes)}};
}

axk::app::Result<void> write_bytes(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(operation_error("volume_package_export_failed", "could not create an export file"));
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output)
        return std::unexpected(operation_error("volume_package_export_failed", "could not write an export file"));
    return {};
}

axk::app::Result<void> write_text(const std::filesystem::path &path, std::string_view value) {
    return write_bytes(path, std::as_bytes(std::span{value}));
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

} // namespace

axk::app::Result<void> axk::app::bind_session_volume_package_operations(OperationRegistry &registry,
                                                                        const Sandbox &sandbox,
                                                                        ImageSessionManager &images,
                                                                        DownloadArchiveStore &downloads) {
    if (!registry.is_implemented("images.volume_package_export.inspect")) {
        auto bound =
            registry.bind("images.volume_package_export.inspect",
                          [&images](const Json &input, const OperationContext &context) -> Result<Json> {
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
                              auto resolved = resolve_context(*content, *session);
                              if (!resolved)
                                  return std::unexpected(resolved.error());
                              return inspection_json(*resolved);
                          });
        if (!bound)
            return bound;
    }
    if (registry.is_implemented("images.volume_package_export"))
        return {};
    return registry.bind(
        "images.volume_package_export",
        [&sandbox, &images, &downloads](const Json &input, const OperationContext &context) -> Result<Json> {
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
            auto resolved = resolve_context(*content, *session);
            if (!resolved)
                return std::unexpected(resolved.error());
            auto context_data = std::move(*resolved);

            std::vector<axk::PackageRootSelector> selectors;
            std::vector<std::size_t> volume_indices;
            for (std::size_t index = 0U; index < context_data.volumes.size(); ++index) {
                const auto &volume = context_data.volumes[index];
                if (volume.object_count == 0U)
                    continue;
                axk::PackageRootSelector selector;
                selector.kind = axk::PackageRootKind::volume;
                selector.partition_index = volume.partition_index;
                selector.volume_directory_id = volume.volume_directory_id;
                selectors.push_back(std::move(selector));
                volume_indices.push_back(index);
            }
            if (selectors.empty()) {
                return std::unexpected(
                    operation_error("volume_package_export_empty", "the selected scope contains no non-empty volumes"));
            }
            report_progress(context, axk::ProgressPhase::resolving, 0U, selectors.size() + 1U,
                            "Building volume packages");
            auto batch = axk::build_portable_packages(*session->media, selectors, context.cancellation);
            if (!batch)
                return std::unexpected(core_error(batch.error()));
            if (batch->packages.empty()) {
                return std::unexpected(
                    operation_error("volume_package_export_empty", "none of the selected volumes could be packaged"));
            }

            Json entries = Json::array();
            for (const auto &volume : context_data.volumes) {
                entries.push_back({{"contentId", volume.content_id},
                                   {"name", volume.name},
                                   {"displayName", volume.display_name},
                                   {"partitionIndex", volume.partition_index},
                                   {"volumeDirectoryId", volume.volume_directory_id},
                                   {"objectCount", volume.object_count},
                                   {"status", volume.object_count == 0U ? "SKIPPED_EMPTY" : "FAILED"},
                                   {"packagePath", Json{}},
                                   {"packageId", Json{}},
                                   {"sizeBytes", Json{}},
                                   {"error", Json{}}});
            }
            for (const auto &failure : batch->failures) {
                const auto volume_index = volume_indices.at(failure.selector_index);
                entries[volume_index]["error"] = {
                    {"code", std::to_string(static_cast<std::uint32_t>(failure.error.code))},
                    {"message", failure.error.message}};
            }
            for (const auto &package : batch->packages) {
                const auto volume_index = volume_indices.at(package.selector_index);
                const auto &volume = context_data.volumes[volume_index];
                entries[volume_index]["status"] = "EXPORTED";
                entries[volume_index]["packagePath"] = volume.package_path;
                entries[volume_index]["packageId"] = package.build.package.package_id;
                entries[volume_index]["sizeBytes"] = package.build.archive.size();
            }

            auto staging = sandbox.create_staging_directory("axklib-volume-packages");
            if (!staging)
                return std::unexpected(staging.error());
            TemporaryDirectoryCleanup cleanup{*staging};
            for (std::size_t index = 0U; index < batch->packages.size(); ++index) {
                const auto &package = batch->packages[index];
                const auto volume_index = volume_indices.at(package.selector_index);
                if (auto written =
                        write_bytes(*staging / context_data.volumes[volume_index].package_path, package.build.archive);
                    !written) {
                    return std::unexpected(written.error());
                }
                report_progress(context, axk::ProgressPhase::writing, index + 1U, batch->packages.size() + 1U,
                                "Writing volume packages");
            }
            const auto skipped_count = static_cast<std::size_t>(
                std::ranges::count(entries, std::string{"SKIPPED_EMPTY"},
                                   [](const Json &entry) { return entry.at("status").get<std::string>(); }));
            const auto failed_count =
                static_cast<std::size_t>(std::ranges::count(entries, std::string{"FAILED"}, [](const Json &entry) {
                    return entry.at("status").get<std::string>();
                }));
            Json report{{"schemaVersion", "1.0"},
                        {"imageId", context_data.image_id},
                        {"revision", context_data.revision},
                        {"source", {{"kind", context_data.source_kind}, {"relativePath", context_data.source_path}}},
                        {"scope", {{"contentId", context_data.scope.id}, {"name", context_data.scope.display_name}}},
                        {"summary",
                         {{"volumeCount", context_data.volumes.size()},
                          {"exportedCount", batch->packages.size()},
                          {"skippedCount", skipped_count},
                          {"failedCount", failed_count}}},
                        {"volumes", entries}};
            const auto report_path = std::string{"volume-packages.axklib.json"};
            const auto report_text = report.dump(2) + '\n';
            if (auto written = write_text(*staging / report_path, report_text); !written)
                return std::unexpected(written.error());
            session->lease.reset();

            Json destination;
            try {
                destination = input.at("destination");
            } catch (const Json::exception &) {
                return std::unexpected(operation_error("invalid_request", "destination is required"));
            }
            Json result{{"imageId", context_data.image_id},
                        {"revision", context_data.revision},
                        {"scopeId", context_data.scope.id},
                        {"volumeCount", context_data.volumes.size()},
                        {"exportedCount", batch->packages.size()},
                        {"skippedCount", skipped_count},
                        {"failedCount", failed_count},
                        {"reportPath", report_path},
                        {"volumes", std::move(entries)}};
            report_progress(context, axk::ProgressPhase::publishing, batch->packages.size(),
                            batch->packages.size() + 1U, "Publishing volume packages");
            const auto kind = destination.value("kind", std::string{});
            if (kind == "WORKSPACE") {
                axk::app::DirectoryRef output;
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
            report_progress(context, axk::ProgressPhase::publishing, batch->packages.size() + 1U,
                            batch->packages.size() + 1U, "Volume packages ready");
            return result;
        });
}
