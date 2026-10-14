#include "image_sessions_internal.hpp"

#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "filesystem_entry_paths.hpp"

namespace axk::app {
Result<std::vector<FilesystemImportDecision>>
ImageSessionManager::inspect_filesystem_import(std::string_view image_id, std::string_view owner_id,
                                               std::uint64_t expected_revision, std::string_view parent_entry_id,
                                               std::span<const FilesystemImportEntry> entries,
                                               const CancellationToken &cancellation) {
    if (entries.empty() || entries.size() > 10000U)
        return std::unexpected(Error{"invalid_request", "Choose between 1 and 10000 import entries"});
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto &state = *session;
    const auto read = begin_filesystem_read(image_id, owner_id, expected_revision);
    if (!read)
        return std::unexpected(read.error());
    const auto parent = detail::FilesystemEntryPaths{*read->filesystem}.resolve(parent_entry_id);
    if (!parent)
        return std::unexpected(parent.error());
    if (parent->entry->kind == "file")
        return std::unexpected(Error{"invalid_request", "The import destination must be a directory"});
    const auto source = Implementation::member(*state, parent->entry->source_member_id);
    if (!source)
        return std::unexpected(source.error());
    if (!(*source)->media || !(*source)->reader)
        return std::unexpected(Error{"unsupported_operation", "The destination is not a writable filesystem"});
    std::vector<FilesystemImportEntry> resolved;
    resolved.reserve(entries.size());
    for (const auto &entry : entries) {
        // An empty relative path must not turn into an edit of the parent itself.
        if (entry.path.empty())
            return std::unexpected(Error{"invalid_request", "Choose a path below the destination directory"});
        auto path = parent->path;
        path.insert(path.end(), entry.path.begin(), entry.path.end());
        resolved.push_back({std::move(path), entry.directory, entry.size_bytes, entry.conflict});
    }
    auto result = (*source)->media->kind() == MediaKind::sfs
                      ? axk::inspect_sfs_file_import((*source)->reader, parent->partition, resolved, cancellation)
                      : axk::inspect_fat_file_import((*source)->reader, parent->partition, resolved, cancellation);
    if (!result)
        return std::unexpected(core_error(result.error(), (*source)->reference));
    if (auto unchanged = read->verify_source_unchanged(); !unchanged)
        return std::unexpected(unchanged.error());
    return std::move(*result);
}
} // namespace axk::app
