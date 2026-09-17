#include "axklib/media.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "media_internal.hpp"

namespace axk {
namespace {
class PartitionReader final : public RandomAccessReader {
  public:
    PartitionReader(std::shared_ptr<const RandomAccessReader> source, std::uint64_t offset, std::uint64_t size)
        : source_(std::move(source)), offset_(offset), size_(size) {}
    std::uint64_t size() const noexcept override { return size_; }
    Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset)
            return std::unexpected{detail::media_error(ErrorCode::out_of_bounds, "read exceeds FAT partition")};
        return source_->read_exact_at(offset_ + offset, destination);
    }

  private:
    std::shared_ptr<const RandomAccessReader> source_;
    std::uint64_t offset_;
    std::uint64_t size_;
};
} // namespace

Result<FatDiskImage> FatDiskImage::open(std::shared_ptr<const RandomAccessReader> reader, std::string source_name,
                                        const CancellationToken &cancellation) {
    if (!reader)
        return std::unexpected{detail::media_error(ErrorCode::invalid_argument, "FAT disk reader is null")};
    const auto header = detail::read_bytes(*reader, 0U, 512U, cancellation);
    if (!header)
        return std::unexpected{header.error()};
    const auto invalid = [&](std::string message) {
        return detail::media_error(ErrorCode::container_invalid_geometry, std::move(message), source_name);
    };
    if ((*header)[510] != std::byte{0x55} || (*header)[511] != std::byte{0xaa})
        return std::unexpected{invalid("MBR signature is invalid")};
    struct Region {
        std::uint8_t number;
        std::uint64_t offset;
        std::uint64_t size;
    };
    std::vector<Region> regions;
    for (std::uint8_t slot = 0; slot < 4U; ++slot) {
        const auto entry = std::span{*header}.subspan(446U + static_cast<std::size_t>(slot) * 16U, 16U);
        const auto type = std::to_integer<std::uint8_t>(entry[4]);
        if (std::ranges::all_of(entry, [](std::byte b) { return b == std::byte{}; }))
            continue;
        if (type != 0x04U && type != 0x06U && type != 0x0eU)
            return std::unexpected{detail::media_error(ErrorCode::unsupported_profile,
                                                       "MBR contains an unsupported non-FAT16 partition", source_name)};
        if (entry[0] != std::byte{} && entry[0] != std::byte{0x80})
            return std::unexpected{invalid("MBR partition boot flag is invalid")};
        const auto offset = static_cast<std::uint64_t>(detail::le32(entry, 8U)) * 512U;
        const auto size = static_cast<std::uint64_t>(detail::le32(entry, 12U)) * 512U;
        if (offset < 512U || size == 0U || offset > reader->size() || size > reader->size() - offset)
            return std::unexpected{invalid("MBR partition exceeds the input image")};
        for (const auto &other : regions)
            if (offset < other.offset + other.size && other.offset < offset + size)
                return std::unexpected{invalid("MBR partitions overlap")};
        regions.push_back({static_cast<std::uint8_t>(slot + 1U), offset, size});
    }
    if (regions.empty())
        return std::unexpected{invalid("MBR contains no FAT16 partitions")};
    FatDiskImage result;
    result.source_name_ = source_name;
    for (const auto &region : regions) {
        auto volume = FatImage::open(std::make_shared<PartitionReader>(reader, region.offset, region.size),
                                     result.source_name_, cancellation);
        if (!volume)
            return std::unexpected{volume.error()};
        if (volume->geometry().profile != FatProfile::fat16)
            return std::unexpected{invalid("MBR FAT16 partition does not contain standard FAT16")};
        result.partitions_.push_back({region.number, region.offset, region.size, std::move(*volume)});
    }
    return result;
}
} // namespace axk
