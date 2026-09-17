#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/filesystem_transaction.hpp"
#include "axklib/media.hpp"

namespace axk::fat_files {
struct Node {
    bool directory{};
    std::string parent;
    std::uint64_t original_offset{};
    std::size_t slot{};
    std::array<std::byte, 32> entry{};
    std::vector<std::uint16_t> clusters;
    std::vector<std::size_t> long_slots;
    std::vector<std::byte> directory_data;
    bool dirty{};
};
struct State {
    std::shared_ptr<const RandomAccessReader> source;
    FatGeometry geometry;
    std::uint64_t base{};
    std::uint64_t region_size{};
    std::vector<std::byte> fat;
    std::map<std::string, Node> nodes;
    std::vector<detail::FilesystemWritePatch> patches;
    CancellationToken cancellation;
    std::size_t directory_bytes{};
    bool fat_dirty{};

    [[nodiscard]] std::uint64_t cluster_offset(std::uint16_t cluster) const;
    [[nodiscard]] Result<std::vector<std::uint16_t>> allocate(std::uint32_t count);
    [[nodiscard]] Result<std::size_t> slot(Node &directory);
    [[nodiscard]] Result<void> apply(const FilesystemEdit &edit);
    [[nodiscard]] Result<void> remove(const std::string &path, bool recursive);
    [[nodiscard]] Result<void> finish();
    [[nodiscard]] Result<void> reserve_directory_bytes(std::size_t bytes);
    void release(Node &node);
    void store(Node &node);
};
[[nodiscard]] Error error(std::string message);
[[nodiscard]] Result<void> check_path(const FilesystemPath &path);
[[nodiscard]] Result<State> open(std::shared_ptr<const RandomAccessReader> source, PartitionIndex partition,
                                 const CancellationToken &cancellation);
void put16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value);
void put32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value);
[[nodiscard]] Result<std::array<std::byte, 11>> short_name(std::string_view name);
} // namespace axk::fat_files
