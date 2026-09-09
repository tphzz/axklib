#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/filesystem_edit.hpp"

namespace axk {

struct FilesystemImportEntry {
    FilesystemPath path;
    bool directory{};
    std::uint64_t size_bytes{};
    FileConflict conflict{FileConflict::skip};
};

enum class FilesystemImportAction : std::uint8_t {
    create_directory,
    merge_directory,
    create_file,
    skip_file,
    replace_file,
    conflict
};

struct FilesystemImportDecision {
    FilesystemImportAction action{FilesystemImportAction::conflict};
    std::optional<std::uint64_t> existing_size_bytes;
    std::string issue;
};

// Ordered path/type review only: no allocation reservation or imported payloads.
// Directories must precede their children. Mutations revalidate all write rules.
[[nodiscard]] AXK_API Result<std::vector<FilesystemImportDecision>>
inspect_sfs_file_import(std::shared_ptr<const RandomAccessReader> source, PartitionIndex partition,
                        std::span<const FilesystemImportEntry> entries, const CancellationToken &cancellation = {});

[[nodiscard]] AXK_API Result<std::vector<FilesystemImportDecision>>
inspect_fat_file_import(std::shared_ptr<const RandomAccessReader> source, PartitionIndex partition,
                        std::span<const FilesystemImportEntry> entries, const CancellationToken &cancellation = {});

} // namespace axk
