#include "filesystem_entry_paths.hpp"
#include "image_sessions_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace axk::app {
namespace {
Result<void> check_relative_path(const FilesystemPath &path) {
    if (path.empty() || path.size() >= 64U)
        return std::unexpected(Error{"invalid_request", "Choose a path below the destination directory"});
    for (const auto &name : path)
        if (name.empty() || name.size() > 255U || name == "." || name == ".." ||
            name.find_first_of("/\\") != std::string::npos || name.find('\0') != std::string::npos)
            return std::unexpected(Error{"invalid_request", "Filesystem names must be individual path components"});
    return {};
}
} // namespace

Result<ResolvedImageFilesystemEdits>
ImageSessionManager::resolve_filesystem_edits(std::string_view image_id, std::string_view owner_id,
                                              std::uint64_t expected_revision,
                                              std::span<const ImageFilesystemEdit> edits) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto &state = *session;
    const std::scoped_lock lock{state->access_mutex};
    if (state->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if (state->mutating || !state->media)
        return std::unexpected(session_error("entry_in_use", "image session is not readable", true));
    if (const auto unchanged = state->verify_source_unchanged(); !unchanged)
        return std::unexpected(unchanged.error());
    if (edits.empty() || edits.size() > 10000U)
        return std::unexpected(session_error("invalid_request", "Choose between 1 and 10000 filesystem changes"));
    std::string move_destination;
    const bool all_moves = std::ranges::all_of(
        edits, [](const auto &item) { return std::holds_alternative<MoveImageFilesystemEntry>(item); });
    for (const auto &edit : edits)
        if (const auto *move = std::get_if<MoveImageFilesystemEntry>(&edit)) {
            if (!all_moves || (!move_destination.empty() && move_destination != move->destination_parent_entry_id))
                return std::unexpected(
                    session_error("invalid_request", "Move batches require one destination and no other edit kinds"));
            move_destination = move->destination_parent_entry_id;
        }
    // Paths resolved from this revision must not be retargeted by earlier renames in a batch.
    if (edits.size() != 1U && std::ranges::any_of(edits, [](const auto &edit) {
            return std::holds_alternative<RenameImageFilesystemEntry>(edit);
        }))
        return std::unexpected(session_error("invalid_request", "Rename requires a single-entry edit request"));
    if (!state->filesystem_index) {
        auto index = detail::build_image_filesystem(*state->media, state->source_reader, state->snapshots_by_id,
                                                    state->descriptors_by_id, state->content);
        if (!index)
            return std::unexpected(index.error());
        state->filesystem_index = std::move(*index);
    }
    const auto &index = *state->filesystem_index;
    if (index.edit_partitions.empty())
        return std::unexpected(
            session_error("unsupported_operation", "Raw editing is unavailable for this filesystem"));
    const detail::FilesystemEntryPaths paths{index};
    ResolvedImageFilesystemEdits result{};
    for (const auto &edit : edits) {
        auto resolved = std::visit(
            [&](const auto &request) -> Result<FilesystemEdit> {
                using Request = std::decay_t<decltype(request)>;
                constexpr bool removing = std::is_same_v<Request, RemoveImageFilesystemEntry>;
                constexpr bool renaming = std::is_same_v<Request, RenameImageFilesystemEntry>;
                constexpr bool moving = std::is_same_v<Request, MoveImageFilesystemEntry>;
                const auto &id = [&]() -> const std::string & {
                    if constexpr (removing || renaming || moving)
                        return request.entry_id;
                    else
                        return request.parent_entry_id;
                }();
                auto selected = paths.resolve(id);
                if (!selected)
                    return std::unexpected(selected.error());
                const auto &entry = *selected->entry;
                if (!result.edits.empty() && result.partition != selected->partition)
                    return std::unexpected(
                        session_error("invalid_request", "Choose entries from one filesystem partition"));
                result.partition = selected->partition;
                if constexpr (removing || renaming || moving) {
                    if (!entry.parent_id)
                        return std::unexpected(session_error("filesystem_entry_protected",
                                                             "Partition roots cannot be deleted, renamed or moved"));
                    if constexpr (renaming)
                        if (auto checked = check_relative_path({request.new_name}); !checked)
                            return std::unexpected(checked.error());
                } else {
                    if (entry.kind == "file")
                        return std::unexpected(session_error("invalid_request", "The destination must be a directory"));
                    if (auto checked = check_relative_path(request.relative_path); !checked)
                        return std::unexpected(checked.error());
                }
                auto path = std::move(selected->path);
                if constexpr (removing) {
                    return RemoveFilesystemEntry{std::move(path), request.recursive};
                } else if constexpr (moving) {
                    auto destination = paths.resolve(request.destination_parent_entry_id);
                    if (!destination)
                        return std::unexpected(destination.error());
                    if (destination->partition != selected->partition || destination->entry->kind == "file")
                        return std::unexpected(
                            session_error("invalid_request", "Choose a directory in the same partition"));
                    const auto capability = std::ranges::find(index.root_capabilities, entry.root_id,
                                                              &ImageFilesystemRootCapabilities::root_id);
                    if (capability == index.root_capabilities.end() || !capability->move_entry)
                        return std::unexpected(
                            session_error("unsupported_operation", "Moving is unavailable for this partition"));
                    return MoveFilesystemEntry{std::move(path), std::move(destination->path)};
                } else if constexpr (renaming) {
                    return RenameFilesystemEntry{std::move(path), request.new_name};
                } else {
                    path.insert(path.end(), request.relative_path.begin(), request.relative_path.end());
                    if constexpr (std::is_same_v<Request, CreateImageFilesystemDirectory>)
                        return CreateFilesystemDirectory{std::move(path)};
                    else {
                        if (!request.contents)
                            return std::unexpected(session_error("invalid_request", "A file input is required"));
                        return PutFilesystemFile{std::move(path), request.contents, request.conflict};
                    }
                }
            },
            edit);
        if (!resolved)
            return std::unexpected(resolved.error());
        result.edits.push_back(std::move(*resolved));
    }
    return result;
}
} // namespace axk::app
