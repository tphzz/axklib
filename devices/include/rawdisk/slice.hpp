// A range of a parent BlockDevice as a BlockDevice of its own: the building block of every
// volume (SCSI2SD disk, later partitions). Slices of slices work.
#pragma once

#include "rawdisk/block_device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace rawdisk {

// Half-open sector range [begin, end).
struct Range {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    constexpr bool empty() const noexcept { return begin >= end; }
    constexpr std::uint64_t size() const noexcept { return empty() ? 0 : end - begin; }
    constexpr bool overlaps(const Range &o) const noexcept {
        return !empty() && !o.empty() && begin < o.end && o.begin < end;
    }
    constexpr bool contains(const Range &o) const noexcept { return o.begin >= begin && o.end <= end; }
    friend constexpr bool operator==(const Range &, const Range &) = default;
};

struct SliceOptions {
    bool writable = false;
    // Parent sectors this slice must never write (checked for the whole write, before anything
    // is written: Errc::layout_conflict).
    std::vector<Range> protected_ranges{};
};

// `count` sectors of `sector_size` bytes, starting at parent sector `first`. sector_size must be
// a multiple of the parent's sector size (at most 65536). The slice must lie inside the parent.
// ReadWrite needs a writable parent (Errc::read_only_handle).
Result<std::shared_ptr<BlockDevice>> make_slice(std::shared_ptr<BlockDevice> parent, std::uint64_t first,
                                                std::uint64_t count, std::uint32_t sector_size,
                                                SliceOptions options = {}) noexcept;

} // namespace rawdisk
