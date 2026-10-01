#include "filesystem_move_batch.hpp"
#include "sfs_files_internal.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <variant>

namespace axk::detail {
Result<PreparedFilesystemEdits> prepare_sfs_file_edits(std::shared_ptr<const RandomAccessReader> source,
                                                       PartitionIndex partition, std::span<const FilesystemEdit> edits,
                                                       const CancellationToken &cancellation) {
    if (!source)
        return std::unexpected(sfs_files::error("filesystem source is required"));
    auto state = sfs_files::open(source, partition, cancellation);
    if (!state)
        return std::unexpected(state.error());
    const auto normalized = normalize_move_batch(edits, false);
    if (!normalized)
        return std::unexpected(normalized.error());
    // Validate even covered descendants and same-parent selections before normalization.
    for (const auto &edit : edits)
        if (const auto *move = std::get_if<MoveFilesystemEntry>(&edit)) {
            auto parent = move->path;
            parent.pop_back();
            if (auto checked = state->apply(MoveFilesystemEntry{move->path, parent}); !checked)
                return std::unexpected(checked.error());
        }
    for (const auto &edit : *normalized)
        if (auto applied = state->apply(edit); !applied)
            return std::unexpected(applied.error());
    if (auto finished = state->finish(); !finished)
        return std::unexpected(finished.error());
    auto patches = normalize_filesystem_patches(
        *source, state->patches, static_cast<std::uint64_t>(state->partition.start_sector) * state->sector_bytes,
        static_cast<std::uint64_t>(state->partition.sector_count) * state->sector_bytes, cancellation);
    if (!patches)
        return std::unexpected(patches.error());
    auto preview = filesystem_preview(source, *patches);
    OpenOptions options;
    options.cancellation = cancellation;
    auto reopened = open_image(preview, {}, options);
    if (!reopened)
        return std::unexpected(reopened.error());
    if (auto checked = sfs_files::validate(*reopened, partition); !checked)
        return std::unexpected(checked.error());
    return PreparedFilesystemEdits{source->size(), std::move(*patches), std::move(preview)};
}
} // namespace axk::detail
