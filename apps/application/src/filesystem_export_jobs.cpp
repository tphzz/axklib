#include "axklib/application/filesystem_export_operations.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "filesystem_export_internal.hpp"

namespace axk::app {
namespace {
using Json = nlohmann::json;

struct Selection {
    std::string image_id;
    std::uint64_t revision;
    std::vector<std::string> entries;
    FilesystemExportLayout layout;
};

Result<Selection> selection(const Json &input) {
    const auto &revision = input.at("expectedRevision");
    if (!revision.is_number_integer() || revision < 1)
        return std::unexpected(Error{"invalid_request", "A positive expectedRevision is required"});
    const auto &ids = input.at("entryIds");
    if (!ids.is_array() || ids.empty() || ids.size() > 10000U)
        return std::unexpected(Error{"invalid_request", "Choose between 1 and 10000 filesystem entries"});
    const auto layout = input.at("layout").get<std::string>();
    if (layout != "EXPORT_FOLDER" && layout != "SELECTED_ENTRIES")
        return std::unexpected(Error{"invalid_request", "Choose an EXPORT_FOLDER or SELECTED_ENTRIES layout"});
    return Selection{
        input.at("imageId").get<std::string>(), revision.get<std::uint64_t>(), ids.get<std::vector<std::string>>(),
        layout == "EXPORT_FOLDER" ? FilesystemExportLayout::export_folder : FilesystemExportLayout::selected_entries};
}

Json summary_json(const Selection &selected, const FilesystemExportSummary &summary) {
    Json entries = Json::array();
    for (const auto &entry : summary.entries)
        entries.push_back({{"entryId", entry.entry_id},
                           {"sourcePath", entry.source_path},
                           {"relativePath", entry.relative_path},
                           {"directory", entry.directory},
                           {"sizeBytes", entry.size_bytes}});
    Json notices = Json::array();
    for (const auto &notice : summary.notices)
        notices.push_back(
            {{"entryId", notice.entry_id}, {"sourcePath", notice.source_path}, {"message", notice.message}});
    return {{"imageId", selected.image_id},
            {"revision", selected.revision},
            {"rootDirectory", summary.root_directory ? Json{{"entryId", summary.root_directory->entry_id},
                                                            {"sourcePath", summary.root_directory->source_path},
                                                            {"name", summary.root_directory->name}}
                                                     : Json(nullptr)},
            {"entries", std::move(entries)},
            {"notices", std::move(notices)},
            {"totalBytes", summary.total_bytes}};
}

bool download_name_valid(const std::string &name) {
    return !name.empty() && name.size() <= 128U && name != "." && name != ".." &&
           std::ranges::none_of(name,
                                [](unsigned char ch) { return ch < 32U || ch == 127U || ch == '/' || ch == '\\'; });
}
} // namespace

Result<void> bind_filesystem_export_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                               ImageSessionManager &images, DownloadArchiveStore &downloads) {
    if (!registry.is_implemented("images.filesystem.export.inspect")) {
        const auto bound = registry.bind(
            "images.filesystem.export.inspect",
            [&images](const Json &input, const OperationContext &context) -> Result<Json> {
                try {
                    const auto selected = selection(input);
                    if (!selected)
                        return std::unexpected(selected.error());
                    const auto result =
                        inspect_filesystem_export(images, selected->image_id, context.owner_id, selected->revision,
                                                  selected->entries, context.cancellation, selected->layout);
                    if (!result)
                        return std::unexpected(result.error());
                    return summary_json(*selected, *result);
                } catch (const Json::exception &) {
                    return std::unexpected(Error{"invalid_request", "Filesystem export selection is malformed"});
                }
            });
        if (!bound)
            return bound;
    }
    if (registry.is_implemented("images.filesystem.export"))
        return {};
    const auto bound = registry.bind(
        "images.filesystem.export",
        [&sandbox, &images, &downloads](const Json &input, const OperationContext &context) -> Result<Json> {
            try {
                const auto selected = selection(input);
                if (!selected)
                    return std::unexpected(selected.error());
                const auto &destination = input.at("destination");
                const auto kind = destination.at("kind").get<std::string>();
                DirectoryRef output;
                std::string directory_name;
                if (kind == "WORKSPACE") {
                    const auto &ref = destination.at("output");
                    output = {ref.at("rootId").get<std::string>(), ref.at("relativePath").get<std::string>()};
                    if (auto resolved = sandbox.resolve_output_directory(output, false); !resolved)
                        return std::unexpected(resolved.error());
                } else if (kind == "DOWNLOAD") {
                    directory_name = destination.at("directoryName").get<std::string>();
                    if (!download_name_valid(directory_name))
                        return std::unexpected(Error{"invalid_request", "Download directory name is invalid"});
                } else
                    return std::unexpected(Error{"invalid_request", "Choose a WORKSPACE or DOWNLOAD destination"});
                auto staged = detail::stage_filesystem_export(images, sandbox, selected->image_id, context.owner_id,
                                                              selected->revision, selected->entries,
                                                              context.cancellation, context.progress, selected->layout);
                if (!staged)
                    return std::unexpected(staged.error());
                auto result = summary_json(*selected, (*staged)->summary);
                result["destination"] = kind;
                result["output"] = nullptr;
                result["download"] = nullptr;
                if (kind == "WORKSPACE") {
                    if (context.progress)
                        context.progress->report(
                            {ProgressPhase::publishing, 0U, 1U, "Publishing filesystem export", {}});
                    if (auto published =
                            sandbox.publish_directory(output, false, (*staged)->path, context.cancellation);
                        !published)
                        return std::unexpected(published.error());
                    result["output"] = {{"rootId", output.root_id}, {"relativePath", output.relative_path}};
                } else {
                    const auto archive =
                        downloads.create_owned_directory(context.owner_id, (*staged)->path, directory_name + ".tar",
                                                         context.cancellation, context.progress);
                    if (!archive)
                        return std::unexpected(archive.error());
                    if (context.cancellation.is_cancelled()) {
                        if (auto removed = downloads.remove(archive->reference, context.owner_id); !removed)
                            return std::unexpected(removed.error());
                        return std::unexpected(Error{"operation_cancelled", "Filesystem export cancelled"});
                    }
                    result["download"] = {
                        {"archiveId", archive->reference.archive_id},
                        {"filename", archive->filename},
                        {"sizeBytes", archive->size_bytes},
                        {"expiresInSeconds", archive->expires_in_seconds},
                        {"contentPath", "/api/v1/download-archives/" + archive->reference.archive_id + "/content"}};
                }
                if (context.progress)
                    context.progress->report({ProgressPhase::publishing, 1U, 1U, "Filesystem export ready", {}});
                return result;
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Filesystem export request is malformed"});
            }
        });
    if (!bound)
        return bound;
    return registry.bind_path_accesses(
        "images.filesystem.export", [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
            try {
                const auto &destination = input.at("destination");
                if (destination.at("kind") == "DOWNLOAD")
                    return std::vector<PathAccess>{};
                if (destination.at("kind") != "WORKSPACE")
                    return std::unexpected(Error{"invalid_request", "Unknown export destination kind"});
                const auto &ref = destination.at("output");
                return std::vector<PathAccess>{
                    {{ref.at("rootId").get<std::string>(), ref.at("relativePath").get<std::string>()},
                     PathAccessMode::exclusive}};
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Filesystem export destination is malformed"});
            }
        });
}
} // namespace axk::app
