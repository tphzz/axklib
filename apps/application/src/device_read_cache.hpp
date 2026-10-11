#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "axklib/io.hpp"

namespace axk::app::device_detail {

class MetadataReadCache {
  public:
    void clear() noexcept {
        pages_.clear();
        recent_.clear();
    }

    template <typename Read, typename Observe>
    axk::Result<void> read(std::uint64_t size, std::uint32_t sector_size, std::uint64_t offset,
                           std::span<std::byte> destination, Read &&physical_read, Observe &&observe) {
        if (sector_size == 0U || sector_size > 64U * 1024U || size % sector_size != 0U || offset > size ||
            destination.size() > size - offset)
            return std::unexpected(
                make_error(ErrorCode::out_of_bounds, ErrorCategory::io, "device read is outside target"));
        const auto page_size = std::max<std::uint32_t>(4096U, sector_size);
        const bool streaming = destination.size() >= 64U * 1024U;
        while (!destination.empty()) {
            const auto alignment = streaming ? sector_size : page_size;
            const auto start = offset - offset % alignment;
            const auto within = static_cast<std::size_t>(offset - start);
            const auto count =
                std::min<std::size_t>(destination.size(), (streaming ? 64U * 1024U : page_size) - within);
            std::vector<std::byte> streamed;
            std::span<const std::byte> data;
            if (streaming) {
                const auto rounded = ((within + count + sector_size - 1U) / sector_size) * sector_size;
                streamed.resize(rounded);
                if (auto loaded = physical_read(start, std::span{streamed}); !loaded)
                    return loaded;
                data = streamed;
            } else {
                auto found = pages_.find(start);
                if (found == pages_.end()) {
                    ++misses;
                    std::vector<std::byte> page(
                        static_cast<std::size_t>(std::min<std::uint64_t>(page_size, size - start)));
                    if (auto loaded = physical_read(start, std::span{page}); !loaded)
                        return loaded;
                    if (pages_.size() >= (4U * 1024U * 1024U) / page_size) {
                        pages_.erase(recent_.back());
                        recent_.pop_back();
                    }
                    recent_.push_front(start);
                    found = pages_.emplace(start, Page{std::move(page), recent_.begin()}).first;
                } else {
                    ++hits;
                    recent_.splice(recent_.begin(), recent_, found->second.recent);
                }
                data = found->second.bytes;
            }
            const auto guard_start = within - within % sector_size;
            const auto guard_end = ((within + count + sector_size - 1U) / sector_size) * sector_size;
            if (auto observed = observe(start + guard_start, data.subspan(guard_start, guard_end - guard_start));
                !observed)
                return observed;
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(within), count, destination.begin());
            destination = destination.subspan(count);
            offset += count;
        }
        return {};
    }

    std::uint64_t hits{};
    std::uint64_t misses{};

  private:
    struct Page {
        std::vector<std::byte> bytes;
        std::list<std::uint64_t>::iterator recent;
    };
    std::list<std::uint64_t> recent_;
    std::map<std::uint64_t, Page> pages_;
};

} // namespace axk::app::device_detail
