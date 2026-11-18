#include "axklib/filesystems/fat/view.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

#include "axklib/media.hpp"

namespace axk::filesystems {
namespace {

class EntryReader final : public RandomAccessReader {
  public:
    EntryReader(std::shared_ptr<const FatImage> disk, std::size_t index) : disk_(std::move(disk)), index_(index) {}

    std::uint64_t size() const noexcept override { return disk_->files()[index_].size; }
    Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        auto bytes = disk_->read_file_range(disk_->files()[index_], offset, destination.size());
        if (!bytes)
            return std::unexpected(bytes.error());
        if (bytes->size() != destination.size())
            return std::unexpected(
                make_error(ErrorCode::io_short_read, ErrorCategory::io, "FAT file read was incomplete"));
        std::ranges::copy(*bytes, destination.begin());
        return {};
    }

  private:
    std::shared_ptr<const FatImage> disk_;
    std::size_t index_;
};

} // namespace

Result<Root> open_fat(std::shared_ptr<const RandomAccessReader> reader, std::string source_name,
                      const CancellationToken &cancellation) {
    auto opened = FatImage::open(std::move(reader), std::move(source_name), cancellation);
    if (!opened)
        return std::unexpected(opened.error());
    return view_fat(std::make_shared<const FatImage>(std::move(*opened)), cancellation);
}

std::string fat_root_id(std::size_t partition) { return std::format("fat-{}", partition); }
std::string fat_entry_id(std::string_view root, bool directory, std::uint64_t directory_offset) {
    return std::format("{}-{}{}", root, directory ? 'd' : 'f', directory_offset);
}

namespace {
Result<Root> project(std::shared_ptr<const FatImage> disk, std::optional<std::size_t> partition,
                     const CancellationToken &cancellation) {
    if (!disk)
        return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::io, "FAT view requires a disk"));
    Root root{partition ? fat_root_id(*partition) : "fat", "FAT", {}};
    const auto id = [&](bool directory, std::uint64_t offset) {
        return partition ? fat_entry_id(root.id, directory, offset) : std::to_string(offset);
    };
    root.entries.reserve(disk->files().size() + disk->directories().size());
    for (const auto &directory : disk->directories()) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        root.entries.push_back({id(true, directory.directory_offset), partition ? "/" + directory.path : directory.path,
                                true, directory.attributes, nullptr});
    }
    for (std::size_t index = 0U; index < disk->files().size(); ++index) {
        const auto &file = disk->files()[index];
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        root.entries.push_back({id(false, file.directory_offset), partition ? "/" + file.path : file.path, false,
                                file.attributes, std::make_shared<EntryReader>(disk, index)});
    }
    return root;
}
} // namespace

Result<Root> view_fat(std::shared_ptr<const FatImage> disk, const CancellationToken &cancellation) {
    return project(std::move(disk), std::nullopt, cancellation);
}
Result<Root> browse_fat(std::shared_ptr<const FatImage> disk, std::size_t partition,
                        const CancellationToken &cancellation) {
    return project(std::move(disk), partition, cancellation);
}

} // namespace axk::filesystems
