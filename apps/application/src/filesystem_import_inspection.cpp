#include "axklib/application/filesystem_edit_operations.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace axk::app {
Result<void> bind_filesystem_import_inspection(OperationRegistry &registry, ImageSessionManager &images) {
    if (registry.is_implemented("images.filesystem.import.inspect"))
        return {};
    return registry.bind(
        "images.filesystem.import.inspect",
        [&images](const nlohmann::json &request, const OperationContext &context) -> Result<nlohmann::json> {
            using Json = nlohmann::json;
            try {
                const auto &revision_value = request.at("expectedRevision");
                if (!revision_value.is_number_integer() || revision_value < 1)
                    return std::unexpected(Error{"invalid_request", "A positive expectedRevision is required"});
                const auto revision = revision_value.get<std::uint64_t>();
                const auto image = request.at("imageId").get<std::string>();
                const auto parent = request.at("parentEntryId").get<std::string>();
                const auto &rows = request.at("entries");
                if (!rows.is_array() || rows.empty() || rows.size() > 10000U)
                    return std::unexpected(Error{"invalid_request", "Choose between 1 and 10000 import entries"});
                std::vector<FilesystemImportEntry> entries;
                entries.reserve(rows.size());
                for (const auto &row : rows) {
                    const auto &size = row.at("sizeBytes");
                    if (!size.is_number_integer() || size < 0 || size > std::numeric_limits<std::uint32_t>::max())
                        return std::unexpected(Error{"invalid_request", "File size is outside the supported bounds"});
                    const auto conflict = row.value("conflict", std::string{"SKIP"});
                    if (conflict != "SKIP" && conflict != "REPLACE")
                        return std::unexpected(Error{"invalid_request", "Choose SKIP or REPLACE for file conflicts"});
                    entries.push_back({row.at("relativePath").get<FilesystemPath>(), row.at("directory").get<bool>(),
                                       size.get<std::uint64_t>(),
                                       conflict == "SKIP" ? FileConflict::skip : FileConflict::replace});
                }
                const auto decisions = images.inspect_filesystem_import(image, context.owner_id, revision, parent,
                                                                        entries, context.cancellation);
                if (!decisions)
                    return std::unexpected(decisions.error());
                static constexpr std::array<std::string_view, 6> actions{
                    "CREATE_DIRECTORY", "MERGE_DIRECTORY", "CREATE_FILE", "SKIP_FILE", "REPLACE_FILE", "CONFLICT"};
                Json result = Json::array();
                std::size_t conflicts{};
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    const auto &decision = decisions->at(i);
                    if (decision.action == FilesystemImportAction::conflict)
                        ++conflicts;
                    result.push_back(
                        {{"relativePath", entries[i].path},
                         {"directory", entries[i].directory},
                         {"sizeBytes", entries[i].size_bytes},
                         {"conflict", entries[i].conflict == FileConflict::skip ? "SKIP" : "REPLACE"},
                         {"action", actions.at(static_cast<std::size_t>(decision.action))},
                         {"existingSizeBytes",
                          decision.existing_size_bytes ? Json(*decision.existing_size_bytes) : Json(nullptr)},
                         {"issue", decision.issue}});
                }
                return Json{{"imageId", image},
                            {"revision", revision},
                            {"parentEntryId", parent},
                            {"entries", std::move(result)},
                            {"conflictCount", conflicts}};
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Filesystem import review is incomplete or malformed"});
            }
        });
}
} // namespace axk::app
