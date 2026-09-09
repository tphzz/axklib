#include "fat_files_internal.hpp"

#include <memory>
#include <span>
#include <utility>

#include "axklib/package_archive.hpp"

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
    const auto snapshot = package_internal::sha256_reader(*source, cancellation);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    for (const auto &edit : edits)
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
    return PreparedFilesystemEdits{source->size(), package_internal::hex_digest(*snapshot), std::move(*patches),
                                   std::move(preview)};
}
} // namespace axk::detail
