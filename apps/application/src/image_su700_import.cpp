#include "image_sessions_internal.hpp"

#include "axklib/filesystem_transaction.hpp"
#include <algorithm>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace axk::app {
Result<ResolvedImageFilesystemEdits>
ImageSessionManager::inspect_su700_import(std::string_view image_id, std::string_view owner_id,
                                          std::uint64_t expected_revision, std::string_view root_entry_id,
                                          std::string_view volume_name, std::span<const FilesystemEdit> edits,
                                          const CancellationToken &cancellation) {
    if (volume_name.empty() || volume_name.size() > 16U || volume_name.front() == ' ' || volume_name.back() == ' ' ||
        volume_name == "." || volume_name == ".." || std::ranges::any_of(volume_name, [](char c) {
            return c < 32 || c > 126 || std::string_view{"/\\:*?\"<>|"}.find(c) != std::string_view::npos;
        }))
        return std::unexpected(
            Error{"invalid_request",
                  "Use 1-16 printable characters for the SU700 volume name, without path separators or edge spaces."});
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto &state = *session;
    const std::scoped_lock lock{state->access_mutex};
    const auto source = implementation_->sandbox.metadata(state->source.root_id, state->source.relative_path);
    if (!implementation_->path_reservations || !source || !source->writable ||
        state->source.kind != ImageSourceKind::file)
        return std::unexpected(Error{"unsupported_operation", "The destination image is read-only."});
    if (state->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "Image changed; inspect the import again.", true));
    if (state->mutating || !state->media)
        return std::unexpected(session_error("entry_in_use", "Image is in use.", true));
    if (auto unchanged = state->verify_source_unchanged(); !unchanged)
        return std::unexpected(unchanged.error());
    if (!state->filesystem_index) {
        auto index = detail::build_image_filesystem(*state->media, state->source_reader, state->snapshots_by_id,
                                                    state->descriptors_by_id, state->content);
        if (!index)
            return std::unexpected(index.error());
        state->filesystem_index = std::move(*index);
    }
    const auto &index = *state->filesystem_index;
    const auto root =
        std::ranges::find(index.root_capabilities, root_entry_id, &ImageFilesystemRootCapabilities::root_id);
    if (root == index.root_capabilities.end() || !std::ranges::contains(root->supported_imports, "SU700_FLOPPY"))
        return std::unexpected(
            Error{"unsupported_operation", "Choose a writable SFS partition containing a recognized SU700 volume."});
    if (std::ranges::any_of(index.entries, [&](const auto &entry) {
            return entry.parent_id == root_entry_id && entry.name == volume_name;
        }))
        return std::unexpected(
            Error{"filesystem_name_conflict", "A directory or file with this volume name already exists."});
    const auto partition = index.edit_partitions.at(std::string{root_entry_id});
    const auto prepared = axk::detail::prepare_sfs_file_edits(state->source_reader, partition, edits, cancellation);
    if (!prepared)
        return std::unexpected(core_error(prepared.error(), state->source));
    if (auto unchanged = state->verify_source_unchanged(); !unchanged)
        return std::unexpected(unchanged.error());
    return ResolvedImageFilesystemEdits{partition, {edits.begin(), edits.end()}};
}
} // namespace axk::app
