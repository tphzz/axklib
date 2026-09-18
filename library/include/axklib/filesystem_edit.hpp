#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"
#include "axklib/publication.hpp"
#include "axklib/types.hpp"

namespace axk {

using FilesystemPath = std::vector<std::string>;
enum class FileConflict : std::uint8_t { skip, replace };

struct CreateFilesystemDirectory {
    FilesystemPath path;
};
struct PutFilesystemFile {
    FilesystemPath path;
    std::shared_ptr<const RandomAccessReader> contents;
    FileConflict conflict{FileConflict::skip};
};
struct RemoveFilesystemEntry {
    FilesystemPath path;
    bool recursive{};
};
struct RenameFilesystemEntry {
    FilesystemPath path;
    std::string new_name;
};
struct MoveFilesystemEntry {
    FilesystemPath path;
    FilesystemPath destination_parent;
};
using FilesystemEdit = std::variant<CreateFilesystemDirectory, PutFilesystemFile, RemoveFilesystemEntry,
                                    RenameFilesystemEntry, MoveFilesystemEntry>;

// Publishes a new destination image. The source and input readers must remain
// immutable for the operation; callers coordinate shared-image path leases.
[[nodiscard]] AXK_API Result<PublicationOutcome>
write_sfs_file_edits(const std::filesystem::path &source, const std::filesystem::path &destination,
                     PartitionIndex partition, std::span<const FilesystemEdit> edits,
                     const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr);

} // namespace axk
