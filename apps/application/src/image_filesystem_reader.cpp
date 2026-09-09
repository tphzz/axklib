#include "image_filesystem_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace axk::app::detail {
Result<std::vector<std::byte>> read_filesystem_range(const MediaContainer &media, const FilesystemFileLocator &file,
                                                     std::uint64_t offset, std::size_t size,
                                                     const CancellationToken &cancellation) {
    if (size > 1024U * 1024U)
        return std::unexpected(Error{"invalid_request", "Filesystem reads are limited to 1 MiB chunks"});
    auto bytes = std::visit(
        [&](const auto &locator) -> axk::Result<std::vector<std::byte>> {
            using T = std::decay_t<decltype(locator)>;
            if constexpr (std::is_same_v<T, SfsFilesystemFile>) {
                if (const auto *sfs = std::get_if<Container>(&media.storage()))
                    return sfs->read_record_range(locator.partition, locator.record, offset, size, cancellation);
            } else if constexpr (std::is_same_v<T, FatFilesystemFile>) {
                const FatImage *fat = std::get_if<FatImage>(&media.storage());
                if (const auto *disk = std::get_if<FatDiskImage>(&media.storage())) {
                    const auto found = std::ranges::find(disk->partitions(), locator.member, &FatDiskPartition::number);
                    if (found != disk->partitions().end())
                        fat = &found->volume;
                } else if (const auto *set = std::get_if<FloppyDiskSet>(&media.storage())) {
                    if (locator.member < set->members().size())
                        fat = &set->members()[locator.member];
                }
                if (fat && locator.ordinal < fat->files().size())
                    return fat->read_file_range(fat->files()[locator.ordinal], offset, size, cancellation);
            } else {
                if (const auto *iso = std::get_if<IsoImage>(&media.storage());
                    iso && locator.ordinal < iso->files().size())
                    return iso->read_file_range(iso->files()[locator.ordinal], offset, size, cancellation);
            }
            return std::unexpected(axk::make_error(ErrorCode::invalid_argument, ErrorCategory::container,
                                                   "Filesystem payload locator is invalid"));
        },
        file);
    if (!bytes)
        return std::unexpected(Error{bytes.error().code == ErrorCode::operation_cancelled ? "operation_cancelled"
                                                                                          : "filesystem_read_failed",
                                     bytes.error().message});
    return std::move(*bytes);
}
} // namespace axk::app::detail
