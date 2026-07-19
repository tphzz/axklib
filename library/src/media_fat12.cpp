#include "axklib/media.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "axklib/utf8.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

bool power_of_two(std::uint32_t value) { return value != 0 && (value & (value - 1U)) == 0; }

std::string decode_fat_name(std::span<const std::byte> entry) {
    auto stem = detail::clean_ascii(entry.subspan(0, 8));
    auto extension = detail::clean_ascii(entry.subspan(8, 3));
    if (std::to_integer<std::uint8_t>(entry[0]) == 0x05U && !stem.empty())
        stem[0] = static_cast<char>(0xe5);
    return extension.empty() ? stem : std::format("{}.{}", stem, extension);
}

Result<std::uint16_t> fat12_entry(std::span<const std::byte> fat, std::uint16_t cluster, std::string_view source) {
    const auto wide_cluster = static_cast<std::uint64_t>(cluster);
    const auto wide_offset = wide_cluster + wide_cluster / 2U;
    if (wide_offset > fat.size() || fat.size() - static_cast<std::size_t>(wide_offset) < 2U) {
        return std::unexpected{detail::media_error(
            ErrorCode::container_invalid_geometry,
            std::format("FAT cannot address cluster {} with {} table bytes", cluster, fat.size()), source)};
    }
    const auto offset = static_cast<std::size_t>(wide_offset);
    const auto pair = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset])) |
                      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset + 1])) << 8U;
    const auto wide_pair = static_cast<std::uint32_t>(pair);
    return (cluster & 1U) != 0U ? static_cast<std::uint16_t>(wide_pair >> 4U)
                                : static_cast<std::uint16_t>(wide_pair & 0x0fffU);
}

Result<std::vector<std::uint16_t>> fat_chain(std::span<const std::byte> fat, const FatGeometry &geometry,
                                             std::uint16_t first, std::optional<std::uint32_t> required_bytes,
                                             std::string_view name, std::string_view source) {
    if (first < 2U || first >= geometry.data_cluster_count + 2U) {
        return std::unexpected{
            detail::media_error(ErrorCode::container_invalid_geometry,
                                std::format("FAT entry '{}' starts at invalid cluster {}", name, first), source)};
    }
    std::vector<std::uint16_t> result;
    std::unordered_set<std::uint16_t> seen;
    auto cluster = first;
    std::uint64_t capacity{};
    while (true) {
        if (cluster < 2U || cluster >= geometry.data_cluster_count + 2U) {
            return std::unexpected{detail::media_error(
                ErrorCode::allocation_invalid_extent,
                std::format("FAT chain for '{}' leaves the data area at cluster {}", name, cluster), source)};
        }
        if (!seen.insert(cluster).second) {
            return std::unexpected{
                detail::media_error(ErrorCode::allocation_cycle,
                                    std::format("FAT chain loop in '{}' at cluster {}", name, cluster), source)};
        }
        result.push_back(cluster);
        capacity += geometry.cluster_size();
        const auto next = fat12_entry(fat, cluster, source);
        if (!next)
            return std::unexpected{next.error()};
        if (*next >= 0xff8U)
            break;
        if (*next == 0xff7U) {
            return std::unexpected{
                detail::media_error(ErrorCode::allocation_invalid_extent,
                                    std::format("FAT chain for '{}' reaches bad cluster marker", name), source)};
        }
        if (*next == 0U || *next == 1U || (*next >= 0xff0U && *next <= 0xff6U)) {
            return std::unexpected{detail::media_error(
                ErrorCode::allocation_invalid_extent,
                std::format("FAT chain for '{}' has invalid successor 0x{:03x}", name, *next), source)};
        }
        cluster = *next;
        if (result.size() > geometry.data_cluster_count) {
            return std::unexpected{
                detail::media_error(ErrorCode::allocation_cycle,
                                    std::format("FAT chain for '{}' exceeds the data cluster count", name), source)};
        }
    }
    if (required_bytes && capacity < *required_bytes) {
        return std::unexpected{detail::media_error(
            ErrorCode::container_truncated,
            std::format("FAT chain for '{}' contains {} bytes but file declares {}", name, capacity, *required_bytes),
            source)};
    }
    return result;
}

std::uint64_t fat_cluster_offset(const FatGeometry &geometry, std::uint16_t cluster) {
    return geometry.data_offset + static_cast<std::uint64_t>(cluster - 2U) * geometry.cluster_size();
}

struct PendingFatDirectory {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint64_t offset{};
};

Result<void> scan_fat_directory(std::vector<FatFile> &files, std::deque<PendingFatDirectory> &pending,
                                std::span<const std::byte> bytes, std::string_view parent,
                                std::uint64_t directory_offset, std::span<const std::byte> fat,
                                const FatGeometry &geometry, const RandomAccessReader &reader, std::string_view source,
                                std::unordered_map<std::uint16_t, std::string> &claimed_clusters,
                                const CancellationToken &cancellation) {
    std::set<std::string> names;
    for (std::size_t offset = 0; offset + 32U <= bytes.size(); offset += 32U) {
        const auto entry = bytes.subspan(offset, 32);
        const auto first = std::to_integer<std::uint8_t>(entry[0]);
        if (first == 0U)
            break;
        if (first == 0xe5U)
            continue;
        const auto attributes = std::to_integer<std::uint8_t>(entry[0x0b]);
        if (attributes == 0x0fU || (attributes & 0x08U) != 0U)
            continue;
        const auto name = decode_fat_name(entry);
        const bool dot = name == "." || name == "..";
        if (!dot && detail::unsafe_component(name)) {
            return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                       std::format("unsafe FAT directory name '{}'", name), source,
                                                       directory_offset + offset)};
        }
        const auto folded = detail::upper_ascii(name);
        if (!dot && !names.insert(folded).second) {
            return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                       std::format("duplicate FAT directory name '{}'", name), source,
                                                       directory_offset + offset)};
        }
        if (dot)
            continue;
        const auto first_cluster = detail::le16(entry, 0x1a);
        const auto size = detail::le32(entry, 0x1c);
        const auto path = parent.empty() ? name : std::format("{}/{}", parent, name);
        if ((attributes & 0x10U) != 0U) {
            const auto chain = fat_chain(fat, geometry, first_cluster, std::nullopt, path, source);
            if (!chain)
                return std::unexpected{chain.error()};
            for (const auto cluster : *chain) {
                const auto [found, inserted] = claimed_clusters.emplace(cluster, path);
                if (!inserted) {
                    return std::unexpected{detail::media_error(
                        ErrorCode::allocation_invalid_extent,
                        std::format("FAT entries '{}' and '{}' share cluster {}", found->second, path, cluster),
                        source)};
                }
            }
            std::vector<std::byte> child;
            child.reserve(chain->size() * geometry.cluster_size());
            for (const auto cluster : *chain) {
                const auto block = detail::read_bytes(reader, fat_cluster_offset(geometry, cluster),
                                                      geometry.cluster_size(), cancellation);
                if (!block)
                    return std::unexpected{block.error()};
                child.insert(child.end(), block->begin(), block->end());
            }
            pending.push_back({path, std::move(child), fat_cluster_offset(geometry, first_cluster)});
            continue;
        }
        if (size == 0U)
            continue;
        const auto chain = fat_chain(fat, geometry, first_cluster, size, path, source);
        if (!chain)
            return std::unexpected{chain.error()};
        for (const auto cluster : *chain) {
            const auto [found, inserted] = claimed_clusters.emplace(cluster, path);
            if (!inserted) {
                return std::unexpected{detail::media_error(
                    ErrorCode::allocation_invalid_extent,
                    std::format("FAT entries '{}' and '{}' share cluster {}", found->second, path, cluster), source)};
            }
        }
        files.push_back({path, name, directory_offset + offset, first_cluster, size, *chain,
                         fat_cluster_offset(geometry, first_cluster)});
    }
    return {};
}

} // namespace

std::uint32_t FatGeometry::cluster_size() const noexcept {
    return static_cast<std::uint32_t>(bytes_per_sector) * sectors_per_cluster;
}

Result<FatImage> FatImage::open(std::shared_ptr<const RandomAccessReader> reader, std::string source_name,
                                const CancellationToken &cancellation) {
    if (!reader)
        return std::unexpected{detail::media_error(ErrorCode::invalid_argument, "FAT reader is null")};
    const auto boot = detail::read_bytes(*reader, 0, 512, cancellation);
    if (!boot)
        return std::unexpected{boot.error()};
    FatGeometry geometry;
    geometry.bytes_per_sector = detail::le16(*boot, 0x0b);
    geometry.sectors_per_cluster = std::to_integer<std::uint8_t>((*boot)[0x0d]);
    geometry.reserved_sectors = detail::le16(*boot, 0x0e);
    geometry.fat_count = std::to_integer<std::uint8_t>((*boot)[0x10]);
    geometry.root_entry_count = detail::le16(*boot, 0x11);
    geometry.total_sectors = detail::le16(*boot, 0x13);
    if (geometry.total_sectors == 0U)
        geometry.total_sectors = detail::le32(*boot, 0x20);
    geometry.media_descriptor = std::to_integer<std::uint8_t>((*boot)[0x15]);
    geometry.sectors_per_fat = detail::le16(*boot, 0x16);
    if (!power_of_two(geometry.bytes_per_sector) || geometry.bytes_per_sector < 512U ||
        geometry.bytes_per_sector > 4096U || !power_of_two(geometry.sectors_per_cluster) ||
        geometry.reserved_sectors == 0U || geometry.fat_count == 0U || geometry.root_entry_count == 0U ||
        geometry.sectors_per_fat == 0U || geometry.total_sectors == 0U) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "invalid or unsupported FAT BPB geometry", source_name)};
    }
    const auto root_sectors =
        (static_cast<std::uint32_t>(geometry.root_entry_count) * 32U + geometry.bytes_per_sector - 1U) /
        geometry.bytes_per_sector;
    const auto metadata_sectors = static_cast<std::uint64_t>(geometry.reserved_sectors) +
                                  static_cast<std::uint64_t>(geometry.fat_count) * geometry.sectors_per_fat +
                                  root_sectors;
    if (metadata_sectors >= geometry.total_sectors ||
        static_cast<std::uint64_t>(geometry.total_sectors) * geometry.bytes_per_sector > reader->size()) {
        return std::unexpected{
            detail::media_error(ErrorCode::container_truncated, "FAT geometry exceeds the input image", source_name)};
    }
    geometry.data_cluster_count =
        static_cast<std::uint32_t>((geometry.total_sectors - metadata_sectors) / geometry.sectors_per_cluster);
    if (geometry.data_cluster_count == 0U || geometry.data_cluster_count >= 4085U) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "unsupported FAT variant: only FAT12 is accepted")};
    }
    geometry.fat_offset = static_cast<std::uint64_t>(geometry.reserved_sectors) * geometry.bytes_per_sector;
    geometry.root_offset =
        static_cast<std::uint64_t>(geometry.reserved_sectors +
                                   static_cast<std::uint32_t>(geometry.fat_count) * geometry.sectors_per_fat) *
        geometry.bytes_per_sector;
    geometry.data_offset = geometry.root_offset + static_cast<std::uint64_t>(root_sectors) * geometry.bytes_per_sector;
    const auto fat_size = static_cast<std::size_t>(geometry.sectors_per_fat) * geometry.bytes_per_sector;
    constexpr std::uint64_t fat12_entry_count = 4096U;
    constexpr std::uint64_t fat12_table_bytes = fat12_entry_count * 3U / 2U;
    const auto maximum_fat_bytes =
        ((fat12_table_bytes + geometry.bytes_per_sector - 1U) / geometry.bytes_per_sector) * geometry.bytes_per_sector;
    const auto highest_data_cluster = static_cast<std::uint64_t>(geometry.data_cluster_count) + 1U;
    const auto highest_entry_offset = highest_data_cluster + highest_data_cluster / 2U;
    if (fat_size > maximum_fat_bytes || highest_entry_offset > fat_size || fat_size - highest_entry_offset < 2U) {
        return std::unexpected{
            detail::media_error(ErrorCode::container_invalid_geometry,
                                std::format("FAT table with {} bytes cannot address {} data clusters", fat_size,
                                            geometry.data_cluster_count),
                                source_name)};
    }
    const auto fat = detail::read_bytes(*reader, geometry.fat_offset, fat_size, cancellation);
    if (!fat)
        return std::unexpected{fat.error()};
    if (fat_size < 3U || std::to_integer<std::uint8_t>((*fat)[0]) != geometry.media_descriptor) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "FAT media descriptor disagrees with the BPB", source_name)};
    }
    for (std::uint8_t index = 1; index < geometry.fat_count; ++index) {
        const auto copy = detail::read_bytes(
            *reader, geometry.fat_offset + static_cast<std::uint64_t>(index) * fat_size, fat_size, cancellation);
        if (!copy)
            return std::unexpected{copy.error()};
        if (*copy != *fat) {
            return std::unexpected{
                detail::media_error(ErrorCode::container_backup_mismatch, "FAT copies disagree", source_name)};
        }
    }
    const auto root_size = static_cast<std::size_t>(root_sectors) * geometry.bytes_per_sector;
    const auto root = detail::read_bytes(*reader, geometry.root_offset, root_size, cancellation);
    if (!root)
        return std::unexpected{root.error()};
    std::vector<FatFile> files;
    std::deque<PendingFatDirectory> pending;
    std::unordered_map<std::uint16_t, std::string> claimed_clusters;
    if (const auto scan = scan_fat_directory(files, pending, *root, {}, geometry.root_offset, *fat, geometry, *reader,
                                             source_name, claimed_clusters, cancellation);
        !scan) {
        return std::unexpected{scan.error()};
    }
    std::unordered_set<std::string> visited_directories;
    while (!pending.empty()) {
        auto directory = std::move(pending.front());
        pending.pop_front();
        if (!visited_directories.insert(detail::upper_ascii(directory.path)).second) {
            return std::unexpected{detail::media_error(
                ErrorCode::allocation_cycle, std::format("duplicate or cyclic FAT directory '{}'", directory.path),
                source_name)};
        }
        if (const auto scan = scan_fat_directory(files, pending, directory.bytes, directory.path, directory.offset,
                                                 *fat, geometry, *reader, source_name, claimed_clusters, cancellation);
            !scan) {
            return std::unexpected{scan.error()};
        }
    }
    std::ranges::sort(files, {}, &FatFile::path);
    FatImage result;
    result.reader_ = std::move(reader);
    result.source_name_ = std::move(source_name);
    result.geometry_ = geometry;
    result.files_ = std::move(files);
    return result;
}

Result<FatImage> FatImage::open(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return open(std::move(*reader), text::path_to_utf8(path), cancellation);
}

const FatGeometry &FatImage::geometry() const noexcept { return geometry_; }
const std::string &FatImage::source_name() const noexcept { return source_name_; }
const std::vector<FatFile> &FatImage::files() const noexcept { return files_; }

Result<std::vector<std::byte>> FatImage::read_file(const FatFile &file, const CancellationToken &cancellation) const {
    return read_file_prefix(file, file.size, cancellation);
}

Result<std::vector<std::byte>> FatImage::read_file_prefix(const FatFile &file, std::size_t maximum_bytes,
                                                          const CancellationToken &cancellation) const {
    return read_file_range(file, 0U, std::min<std::size_t>(file.size, maximum_bytes), cancellation);
}

Result<std::vector<std::byte>> FatImage::read_file_range(const FatFile &file, std::uint64_t offset, std::size_t size,
                                                         const CancellationToken &cancellation) const {
    if (offset > file.size || size > file.size - offset) {
        return std::unexpected{detail::media_error(
            ErrorCode::out_of_bounds, std::format("FAT file range exceeds '{}'", file.path), source_name_)};
    }
    std::vector<std::byte> result;
    result.reserve(size);
    auto remaining = size;
    const auto cluster_size = geometry_.cluster_size();
    auto cluster_index = static_cast<std::size_t>(offset / cluster_size);
    auto within_cluster = static_cast<std::size_t>(offset % cluster_size);
    while (remaining > 0U && cluster_index < file.clusters.size()) {
        const auto cluster = file.clusters[cluster_index];
        const auto take = std::min<std::size_t>(remaining, cluster_size - within_cluster);
        const auto bytes =
            detail::read_bytes(*reader_, fat_cluster_offset(geometry_, cluster) + within_cluster, take, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        result.insert(result.end(), bytes->begin(), bytes->end());
        remaining -= take;
        ++cluster_index;
        within_cluster = 0U;
    }
    if (remaining != 0U) {
        return std::unexpected{detail::media_error(ErrorCode::container_truncated,
                                                   std::format("FAT file '{}' is truncated", file.path), source_name_)};
    }
    return result;
}

} // namespace axk
