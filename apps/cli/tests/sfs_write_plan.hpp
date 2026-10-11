#pragma once

#include "axklib/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace axk::test::sfs_write {
inline constexpr std::uint64_t exclusion_sectors = 16U * 1024U * 1024U / 512U;
inline constexpr std::uint64_t maximum_search_sectors = 64U * 1024U * 1024U / 512U;
struct Guard {
    std::uint64_t lba{};
    std::vector<std::byte> bytes;
};
struct Plan {
    std::uint64_t sector_count{};
    std::uint64_t lba{};
    std::uint64_t free_begin{};
    std::uint64_t free_end{};
    std::vector<std::byte> original;
    std::vector<Guard> guards;
};
// Test utility only. Regular full-target backup files, read-only; never accepts device paths.
Result<Plan> prepare(const std::filesystem::path &backup);
Result<void> save(const std::filesystem::path &new_file, const Plan &plan);
} // namespace axk::test::sfs_write
