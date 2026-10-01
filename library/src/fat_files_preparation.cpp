#include "fat_files_internal.hpp"
#include "filesystem_move_batch.hpp"

#include <memory>
#include <span>
#include <utility>
#include <variant>

namespace axk::detail {
Result<void> inspect_fat_file_edit_support(std::shared_ptr<const RandomAccessReader> source, PartitionIndex partition,
                                           const CancellationToken &cancellation) {
    auto state = fat_files::open(std::move(source), partition, cancellation);
    if (!state)
        return std::unexpected(state.error());
    return {};
}
Result<PreparedFilesystemEdits> prepare_fat_file_edits(std::shared_ptr<const RandomAccessReader> source,
                                                       PartitionIndex partition, std::span<const FilesystemEdit> edits,
                                                       const CancellationToken &cancellation) {
    if (edits.size() > 10000U)
        return std::unexpected(fat_files::error("FAT edit count exceeds its limit"));
    auto state = fat_files::open(source, partition, cancellation);
    if (!state)
        return std::unexpected(state.error());
    const auto normalized = normalize_move_batch(edits, true);
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
    auto patches = normalize_filesystem_patches(*source, state->patches, state->base, state->region_size, cancellation);
    if (!patches)
        return std::unexpected(patches.error());
    auto preview = filesystem_preview(source, *patches);
    if (auto checked = fat_files::open(preview, partition, cancellation); !checked)
        return std::unexpected(checked.error());
    return PreparedFilesystemEdits{source->size(), std::move(*patches), std::move(preview)};
}
} // namespace axk::detail
