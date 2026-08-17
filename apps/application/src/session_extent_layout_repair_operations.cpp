#include "axklib/application/session_extent_layout_repair_operations.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/sfs_repair.hpp"
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
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "extent_layout_repair_failed",
            error.message};
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

Json extent_json(const axk::Extent &extent) {
    return {{"clusterOffset", extent.cluster_offset},
            {"clusterCount", extent.cluster_count},
            {"byteCount", extent.byte_count}};
}

Json repair_json(const axk::SfsExtentLayoutRepairTarget &repair) {
    Json source_extents = Json::array();
    for (const auto &extent : repair.source_extents)
        source_extents.push_back(extent_json(extent));
    Json replacement_extents = Json::array();
    for (const auto &extent : repair.replacement_extents)
        replacement_extents.push_back(extent_json(extent));
    return {{"partitionIndex", repair.partition.value},
            {"recordId", repair.record.value},
            {"logicalSize", repair.logical_size},
            {"sourceExtents", std::move(source_extents)},
            {"replacementExtents", std::move(replacement_extents)}};
}

std::string default_filename(const axk::app::ImageSourceRef &source) {
    const auto source_path = axk::text::path_from_utf8(source.relative_path);
    if (!source_path)
        return "repaired.hds";
    const auto extension = source_path->extension().empty() ? std::filesystem::path{".hds"} : source_path->extension();
    return axk::text::path_to_utf8(source_path->stem()) + "_repaired" + axk::text::path_to_utf8(extension);
}

axk::app::Result<axk::app::FileRef> parse_workspace_output(const Json &destination) {
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

axk::app::Result<Json> publish_result(const axk::app::Sandbox &sandbox, axk::app::DownloadArchiveStore &downloads,
                                      const axk::app::OperationContext &context, const Json &destination,
                                      const std::filesystem::path &staged_file, std::string_view suggested_filename,
                                      Json result) {
    const auto kind = destination.value("kind", std::string{});
    if (kind == "WORKSPACE") {
        auto output = parse_workspace_output(destination);
        if (!output)
            return std::unexpected(output.error());
        auto reader = axk::FileReader::open(staged_file);
        if (!reader)
            return std::unexpected(core_error(reader.error()));
        if (auto published = sandbox.publish_file(*output, destination.value("overwrite", false), **reader); !published)
            return std::unexpected(published.error());
        result["destination"] = "WORKSPACE";
        result["output"] = {{"rootId", output->root_id}, {"relativePath", output->relative_path}};
        result["download"] = nullptr;
        return result;
    }
    if (kind == "DOWNLOAD") {
        const auto filename = destination.value("filename", std::string{suggested_filename});
        if (filename.empty() || filename == "." || filename == ".." ||
            filename.find_first_of("/\\") != std::string::npos)
            return std::unexpected(operation_error("invalid_request", "download filename is invalid"));
        auto retained = downloads.retain_owned_file(context.owner_id, staged_file, filename, "application/octet-stream",
                                                    context.cancellation, context.progress);
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
        return result;
    }
    return std::unexpected(operation_error("invalid_request", "destination kind must be WORKSPACE or DOWNLOAD"));
}

} // namespace

axk::app::Result<void> axk::app::bind_session_extent_layout_repair_operations(OperationRegistry &registry,
                                                                              const Sandbox &sandbox,
                                                                              ImageSessionManager &images,
                                                                              DownloadArchiveStore &downloads) {
    if (registry.is_implemented("images.extent_layout.repair"))
        return {};
    return registry.bind(
        "images.extent_layout.repair",
        [&sandbox, &images, &downloads](const Json &input, const OperationContext &context) -> Result<Json> {
            const auto identity = parse_identity(input);
            if (!identity)
                return std::unexpected(identity.error());
            auto session = images.begin_read(identity->first, context.owner_id, identity->second);
            if (!session)
                return std::unexpected(session.error());
            if (session->source.kind != ImageSourceKind::file || session->media == nullptr)
                return std::unexpected(operation_error("extent_layout_repair_unsupported",
                                                       "Only file-backed SFS image sessions support this repair"));
            const auto *container = std::get_if<axk::Container>(&session->media->storage());
            if (container == nullptr)
                return std::unexpected(
                    operation_error("extent_layout_repair_unsupported", "Only SFS image sessions support this repair"));
            const auto target = axk::inspect_sfs_extent_layout_repair(*container);
            if (!target)
                return std::unexpected(core_error(target.error()));

            const auto source_ref = FileRef{session->source.root_id, session->source.relative_path};
            const auto source_path = sandbox.resolve_file(source_ref);
            if (!source_path)
                return std::unexpected(source_path.error());
            const auto suggested_filename = default_filename(session->source);
            auto staging = sandbox.create_staging_directory("axklib-extent-layout-repair");
            if (!staging)
                return std::unexpected(staging.error());
            TemporaryDirectoryCleanup cleanup{*staging};
            const auto staged_file = *staging / "repaired-image.hds";
            if (context.progress)
                context.progress->report(
                    {axk::ProgressPhase::writing, 0U, 2U, "Repairing SFS extent layout", std::nullopt});
            const auto repaired =
                axk::repair_sfs_extent_layout(*source_path, staged_file, context.cancellation, context.progress, false);
            if (!repaired)
                return std::unexpected(core_error(repaired.error()));
            session->lease.reset();

            Json destination;
            try {
                destination = input.at("destination");
            } catch (const Json::exception &) {
                return std::unexpected(operation_error("invalid_request", "destination is required"));
            }
            Json repairs = Json::array();
            for (const auto &repair : repaired->repairs)
                repairs.push_back(repair_json(repair));
            std::error_code size_error;
            const auto size_bytes = std::filesystem::file_size(staged_file, size_error);
            if (size_error)
                return std::unexpected(
                    operation_error("extent_layout_repair_failed", "Could not determine repaired image size"));
            Json result{{"imageId", identity->first},
                        {"revision", identity->second},
                        {"repairs", std::move(repairs)},
                        {"sizeBytes", size_bytes},
                        {"defaultFilename", suggested_filename}};
            if (context.progress)
                context.progress->report(
                    {axk::ProgressPhase::publishing, 1U, 2U, "Publishing repaired image copy", std::nullopt});
            auto published = publish_result(sandbox, downloads, context, destination, staged_file, suggested_filename,
                                            std::move(result));
            if (!published)
                return std::unexpected(published.error());
            if (context.progress)
                context.progress->report(
                    {axk::ProgressPhase::publishing, 2U, 2U, "Repaired image copy ready", std::nullopt});
            return published;
        });
}
