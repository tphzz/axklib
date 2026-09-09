#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "axklib/application/image_filesystem.hpp"
#include "axklib/application/image_sessions.hpp"

namespace axk::app::detail {

struct SfsFilesystemFile {
    PartitionIndex partition;
    SfsId record;
};
struct FatFilesystemFile {
    std::size_t member{};
    std::size_t ordinal{};
};
struct IsoFilesystemFile {
    std::size_t ordinal{};
};
using FilesystemFileLocator = std::variant<SfsFilesystemFile, FatFilesystemFile, IsoFilesystemFile>;

struct ImageFilesystemIndex {
    bool available{};
    std::string filesystem_name;
    std::optional<std::string> device_view;
    std::vector<ImageFilesystemEntry> entries;
    std::unordered_map<std::string, PartitionIndex> edit_partitions;
    std::vector<ImageFilesystemRootCapabilities> root_capabilities;
    std::unordered_map<std::string, FilesystemFileLocator> files;
};

void identify_su700_import_roots(ImageFilesystemIndex &index, const MediaContainer &media);

[[nodiscard]] Result<ImageFilesystemIndex>
build_image_filesystem(const MediaContainer &media, std::shared_ptr<const RandomAccessReader> source,
                       const std::unordered_map<std::string, ObjectSnapshot> &objects,
                       const std::unordered_map<std::string, MediaObjectDescriptor> &descriptors,
                       const std::vector<ImageContentItem> &content);

[[nodiscard]] Result<std::vector<std::byte>> read_filesystem_range(const MediaContainer &media,
                                                                   const FilesystemFileLocator &file,
                                                                   std::uint64_t offset, std::size_t size,
                                                                   const CancellationToken &cancellation);

} // namespace axk::app::detail
