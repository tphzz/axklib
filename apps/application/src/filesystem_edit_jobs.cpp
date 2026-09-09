#include "axklib/application/filesystem_edit_operations.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/su700_import_operations.hpp"
#include "filesystem_inputs.hpp"

namespace axk::app {
namespace {
using Json = nlohmann::json;

} // namespace

Result<void> bind_filesystem_edit_operations(OperationRegistry &registry, const Sandbox &sandbox, UploadStore &uploads,
                                             ImageSessionManager &images, AlterationJournalStore &journals) {
    if (auto bound = filesystem_inputs::bind_inspection(registry, sandbox, uploads); !bound)
        return bound;
    if (auto bound = bind_filesystem_import_inspection(registry, images); !bound)
        return bound;
    if (auto bound = bind_su700_import_operations(registry, sandbox, uploads, images, journals); !bound)
        return bound;
    if (registry.is_implemented("images.filesystem.edit"))
        return {};
    auto bound = registry.bind(
        "images.filesystem.edit",
        [&sandbox, &uploads, &images, &journals](const Json &input, const OperationContext &context) -> Result<Json> {
            try {
                if (!input.at("acknowledgeDeviceRelationships").get<bool>())
                    return std::unexpected(
                        Error{"invalid_request", "Confirm that raw changes may break sampler relationships"});
                const auto image_id = input.at("imageId").get<std::string>();
                const auto &revision_value = input.at("expectedRevision");
                if (!revision_value.is_number_integer() || revision_value < 1)
                    return std::unexpected(Error{"invalid_request", "A positive expectedRevision is required"});
                const auto revision = revision_value.get<std::uint64_t>();
                const auto &rows = input.at("edits");
                if (!rows.is_array() || rows.empty() || rows.size() > 10000U)
                    return std::unexpected(Error{"invalid_request", "Choose between 1 and 10000 filesystem changes"});
                std::vector<std::pair<filesystem_inputs::OpenedInput, Json>> inputs;
                std::vector<ImageFilesystemEdit> requests;
                for (const auto &row : rows) {
                    if (auto checked = context.cancellation.check(); !checked)
                        return std::unexpected(Error{"operation_cancelled", "Filesystem editing was cancelled"});
                    const auto kind = row.at("kind").get<std::string>();
                    if (kind == "DELETE") {
                        requests.emplace_back(RemoveImageFilesystemEntry{row.at("entryId").get<std::string>(),
                                                                         row.at("recursive").get<bool>()});
                    } else if (kind == "CREATE_DIRECTORY" || kind == "PUT_FILE") {
                        const auto parent = row.at("parentEntryId").get<std::string>();
                        const auto path = row.at("relativePath").get<FilesystemPath>();
                        if (kind == "CREATE_DIRECTORY")
                            requests.emplace_back(CreateImageFilesystemDirectory{parent, path});
                        else {
                            const auto conflict = row.value("conflict", std::string{"SKIP"});
                            if (conflict != "SKIP" && conflict != "REPLACE")
                                return std::unexpected(
                                    Error{"invalid_request", "Choose SKIP or REPLACE for file conflicts"});
                            auto file = filesystem_inputs::open(row.at("source"), context.owner_id, sandbox, uploads);
                            if (!file)
                                return std::unexpected(file.error());
                            const auto &expected = row.at("expectedSource");
                            if (auto checked = file->verify(expected, context.cancellation); !checked)
                                return std::unexpected(checked.error());
                            requests.emplace_back(PutImageFilesystemFile{parent, path, file->reader,
                                                                         conflict == "SKIP" ? FileConflict::skip
                                                                                            : FileConflict::replace});
                            inputs.emplace_back(std::move(*file), expected);
                        }
                    } else
                        return std::unexpected(Error{"invalid_request", "Unknown filesystem edit kind"});
                }
                auto resolved = images.resolve_filesystem_edits(image_id, context.owner_id, revision, requests);
                if (!resolved)
                    return std::unexpected(resolved.error());
                const auto validate_inputs = [&]() -> Result<void> {
                    for (const auto &[file, expected] : inputs) {
                        if (auto checked = file.verify(expected); !checked)
                            return checked;
                    }
                    return {};
                };
                auto result =
                    apply_filesystem_edits(images, journals, image_id, context.owner_id, revision, resolved->partition,
                                           resolved->edits, context.cancellation, context.progress, validate_inputs);
                if (!result)
                    return std::unexpected(result.error());
                return Json{{"imageId", result->image_id},
                            {"revision", result->revision},
                            {"warnings", Json::array({"Raw filesystem changes do not repair sampler relationships."})}};
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Filesystem edit request is incomplete or malformed"});
            }
        });
    if (!bound)
        return bound;
    return registry.bind_path_accesses(
        "images.filesystem.edit", [](const Json &input, const OperationContext &) -> Result<std::vector<PathAccess>> {
            try {
                std::vector<PathAccess> accesses;
                for (const auto &edit : input.at("edits")) {
                    if (edit.value("kind", std::string{}) != "PUT_FILE")
                        continue;
                    const auto &source = edit.at("source");
                    if (source.contains("fileRef")) {
                        const auto &ref = source.at("fileRef");
                        accesses.push_back(
                            {{ref.at("rootId").get<std::string>(), ref.at("relativePath").get<std::string>()},
                             PathAccessMode::shared});
                    }
                }
                return accesses;
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Filesystem input reference is malformed"});
            }
        });
}
} // namespace axk::app
