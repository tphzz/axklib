#include "axklib/application/directory_archive_operations.hpp"

#include <string>

#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/filesystem.hpp"

namespace {

using Json = nlohmann::json;

axk::app::Result<axk::app::DirectoryRef> directory_ref(const Json &input) {
    try {
        const auto &value = input.at("directory");
        auto result =
            axk::app::DirectoryRef{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
        if (result.root_id.empty())
            return std::unexpected(axk::app::Error{"invalid_request", "directory rootId is required"});
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(axk::app::Error{"invalid_request", "directory must be one sandbox DirectoryRef"});
    }
}

} // namespace

axk::app::Result<void> axk::app::bind_directory_archive_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                                   DownloadArchiveStore &downloads) {
    if (auto bound = registry.bind(
            "files.archive",
            [&sandbox, &downloads](const Json &input, const OperationContext &context) -> Result<Json> {
                const auto directory = directory_ref(input);
                if (!directory)
                    return std::unexpected(directory.error());
                const auto archive =
                    downloads.create(context.owner_id, sandbox, *directory, context.cancellation, context.progress);
                if (!archive)
                    return std::unexpected(archive.error());
                return Json{{"archiveId", archive->reference.archive_id},
                            {"filename", archive->filename},
                            {"sizeBytes", archive->size_bytes},
                            {"entryCount", archive->entry_count},
                            {"expiresInSeconds", archive->expires_in_seconds},
                            {"contentPath", "/api/v1/download-archives/" + archive->reference.archive_id + "/content"}};
            });
        !bound) {
        return bound;
    }
    return registry.bind_path_accesses(
        "files.archive", [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
            const auto directory = directory_ref(input);
            if (!directory)
                return std::unexpected(directory.error());
            return std::vector<PathAccess>{{{directory->root_id, directory->relative_path}, PathAccessMode::shared}};
        });
}
