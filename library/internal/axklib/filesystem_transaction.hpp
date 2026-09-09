#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "axklib/filesystem_edit.hpp"
#include "axklib/sfs.hpp"

namespace axk::detail {

[[nodiscard]] Result<void> inspect_sfs_file_edit_support(const Container &container, PartitionIndex partition);
[[nodiscard]] Result<void> inspect_fat_file_edit_support(std::shared_ptr<const RandomAccessReader> source,
                                                         PartitionIndex partition,
                                                         const CancellationToken &cancellation = {});

struct FilesystemWritePatch {
    std::uint64_t offset{};
    std::shared_ptr<const RandomAccessReader> source;
    std::uint64_t source_offset{};
    std::uint64_t size{};
};

struct PreparedFilesystemEdits {
    std::uint64_t image_size_bytes{};
    std::string source_snapshot_id;
    std::vector<FilesystemWritePatch> patches;
    std::shared_ptr<const RandomAccessReader> preview;
};

[[nodiscard]] Result<std::vector<FilesystemWritePatch>>
normalize_filesystem_patches(const RandomAccessReader &source, std::span<const FilesystemWritePatch> patches,
                             std::uint64_t offset, std::uint64_t size, const CancellationToken &cancellation);
[[nodiscard]] std::shared_ptr<const RandomAccessReader>
filesystem_preview(std::shared_ptr<const RandomAccessReader> source, std::vector<FilesystemWritePatch> patches);

// Patches are ordered, disjoint and confined to the selected partition.
// Source and input readers must remain immutable until a caller freezes these
// ranges in its transaction journal. This function never writes the source.
[[nodiscard]] Result<PreparedFilesystemEdits> prepare_sfs_file_edits(std::shared_ptr<const RandomAccessReader> source,
                                                                     PartitionIndex partition,
                                                                     std::span<const FilesystemEdit> edits,
                                                                     const CancellationToken &cancellation = {});

// The same immutable-reader contract applies. PartitionIndex is the zero-based
// ordinal in FatDiskImage::partitions(), not its one-based MBR slot number.
[[nodiscard]] Result<PreparedFilesystemEdits> prepare_fat_file_edits(std::shared_ptr<const RandomAccessReader> source,
                                                                     PartitionIndex partition,
                                                                     std::span<const FilesystemEdit> edits,
                                                                     const CancellationToken &cancellation = {});

} // namespace axk::detail
