#pragma once

#include "rawdisk/probe.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace rawdisk::probe::detail {
inline constexpr std::size_t kMaxBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxGuards = 4096;
Result<std::vector<std::byte>> read_file(const std::filesystem::path &path);
Result<void> write_new(const std::filesystem::path &path, std::span<const std::byte> bytes);
Result<void> sync_directory(const std::filesystem::path &path);
Result<void> publication_barrier(const std::filesystem::path &file);
Result<void> save_journal(const std::filesystem::path &directory, const Journal &journal);
} // namespace rawdisk::probe::detail
