#include "media_ex5_internal.hpp"

#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "media_internal.hpp"

namespace axk::detail {

Result<FatGeometry> read_ex5_geometry(const RandomAccessReader &reader, std::string_view source,
                                      const CancellationToken &cancellation) {
    constexpr std::uint64_t boot_sector = 256U;
    constexpr std::uint32_t sector_bytes = 512U;
    const auto boot = read_bytes(reader, boot_sector * sector_bytes, sector_bytes, cancellation);
    if (!boot)
        return std::unexpected{boot.error()};
    FatGeometry geometry;
    geometry.profile = FatProfile::ex5_disk;
    geometry.boot_offset = boot_sector * sector_bytes;
    geometry.bytes_per_sector = le16(*boot, 11U);
    geometry.sectors_per_cluster = std::to_integer<std::uint8_t>((*boot)[13U]);
    geometry.reserved_sectors = le16(*boot, 14U);
    geometry.fat_count = std::to_integer<std::uint8_t>((*boot)[16U]);
    geometry.root_entry_count = le16(*boot, 17U);
    geometry.data_cluster_count = le16(*boot, 19U);
    geometry.media_descriptor = std::to_integer<std::uint8_t>((*boot)[21U]);
    geometry.sectors_per_fat = le16(*boot, 22U);
    const auto cluster_sectors = geometry.sectors_per_cluster;
    if (geometry.bytes_per_sector != sector_bytes || cluster_sectors == 0U || cluster_sectors > 64U ||
        (cluster_sectors & (cluster_sectors - 1U)) != 0U || geometry.reserved_sectors != 2U ||
        geometry.fat_count != 2U || geometry.root_entry_count == 0U || geometry.root_entry_count % 16U != 0U ||
        geometry.sectors_per_fat == 0U || geometry.data_cluster_count == 0U || geometry.data_cluster_count > 0xfffdU ||
        geometry.media_descriptor != 0xf8U || le16(*boot, 24U) != 32U || le16(*boot, 26U) != 8U ||
        clean_ascii(std::span{*boot}.subspan(54U, 8U)) != "FAT16" || (*boot)[510U] != std::byte{0x55} ||
        (*boot)[511U] != std::byte{0xaa}) {
        return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                           "invalid or unsupported EX5 disk geometry", source, geometry.boot_offset)};
    }
    const auto root_sectors =
        (static_cast<std::uint32_t>(geometry.root_entry_count) * 32U + sector_bytes - 1U) / sector_bytes;
    const auto fat_sectors = static_cast<std::uint32_t>(geometry.fat_count) * geometry.sectors_per_fat;
    const auto data_sectors = static_cast<std::uint64_t>(geometry.data_cluster_count) * cluster_sectors;
    // EX5 stores cluster count at +19 and capacity excluding prefix/reserved/root at +32.
    const auto capacity_sectors = static_cast<std::uint64_t>(le32(*boot, 32U));
    const auto declared_end = boot_sector + geometry.reserved_sectors + root_sectors + capacity_sectors;
    const auto data_start = boot_sector + geometry.reserved_sectors + fat_sectors + root_sectors;
    if (capacity_sectors < fat_sectors + data_sectors || declared_end * sector_bytes > reader.size() ||
        (data_start + data_sectors) * sector_bytes > reader.size()) {
        return std::unexpected{
            media_error(ErrorCode::container_truncated, "EX5 capacity or data region exceeds the input image", source)};
    }
    geometry.total_sectors = static_cast<std::uint32_t>(data_start + data_sectors);
    geometry.fat_offset = (boot_sector + geometry.reserved_sectors) * sector_bytes;
    geometry.root_offset = (boot_sector + geometry.reserved_sectors + fat_sectors) * sector_bytes;
    geometry.data_offset = data_start * sector_bytes;
    return geometry;
}

ContentTree ex5_content_tree(const FatImage &image) {
    std::map<std::string, std::vector<ContentNode>> children;
    const auto parent = [](std::string_view path) {
        const auto slash = path.rfind('/');
        return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash)};
    };
    for (const auto &file : image.files()) {
        ContentNode node{std::format("file:{}", file.path), "file", file.name};
        node.details.push_back(std::format("{} bytes", file.size));
        children[parent(file.path)].push_back(std::move(node));
    }
    // Reverse path order visits descendants before parents without recursive traversal.
    for (auto directory = image.directories().rbegin(); directory != image.directories().rend(); ++directory) {
        ContentNode node{std::format("directory:{}", directory->path), "directory", directory->name};
        node.children = std::move(children[directory->path]);
        children[parent(directory->path)].push_back(std::move(node));
    }
    const bool ex5 = image.geometry().profile != FatProfile::fat16;
    ContentNode root{ex5 ? "ex5-root" : "fat16-root", "directory", ex5 ? "EX5 disk" : "FAT16 volume"};
    root.children = std::move(children[""]);
    ContentTree result;
    result.source_path = image.source_name();
    result.roots.push_back(std::move(root));
    return result;
}

} // namespace axk::detail
