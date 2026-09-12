#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t sector_bytes = 512U;
constexpr std::size_t boot_offset = 256U * sector_bytes;
constexpr std::size_t fat_offset = 258U * sector_bytes;
constexpr std::size_t fat_bytes = 17U * sector_bytes;
constexpr std::size_t root_offset = fat_offset + 2U * fat_bytes;
constexpr std::size_t data_offset = root_offset + sector_bytes;

inline void le16(std::vector<std::byte> &bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

inline void le32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
    le16(bytes, offset, static_cast<std::uint16_t>(value & 0xffffU));
    le16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

inline void ascii(std::vector<std::byte> &bytes, std::size_t offset, std::string_view text) {
    for (const auto c : text)
        bytes[offset++] = static_cast<std::byte>(c);
}

inline void fat_entry(std::vector<std::byte> &bytes, std::uint16_t cluster, std::uint16_t next) {
    le16(bytes, fat_offset + static_cast<std::size_t>(cluster) * 2U, next);
    le16(bytes, fat_offset + fat_bytes + static_cast<std::size_t>(cluster) * 2U, next);
}

inline std::vector<std::byte> ex5_fixture() {
    std::vector<std::byte> bytes(data_offset + 4096U * sector_bytes);
    ascii(bytes, 0, "YAMAHA_dev3");
    ascii(bytes, 0x210U, "SY1200 V0.0.0   ");
    ascii(bytes, boot_offset + 3U, "YAMAHA??");
    le16(bytes, boot_offset + 11U, 512U);
    bytes[boot_offset + 13U] = std::byte{1};
    le16(bytes, boot_offset + 14U, 2U);
    bytes[boot_offset + 16U] = std::byte{2};
    le16(bytes, boot_offset + 17U, 16U);
    le16(bytes, boot_offset + 19U, 4096U);
    bytes[boot_offset + 21U] = std::byte{0xf8};
    le16(bytes, boot_offset + 22U, 17U);
    le16(bytes, boot_offset + 24U, 32U);
    le16(bytes, boot_offset + 26U, 8U);
    le32(bytes, boot_offset + 32U, 4096U + 34U);
    ascii(bytes, boot_offset + 54U, "FAT16   ");
    bytes[boot_offset + 510U] = std::byte{0x55};
    bytes[boot_offset + 511U] = std::byte{0xaa};
    fat_entry(bytes, 0, 0xfff8U);
    fat_entry(bytes, 1, 0xffffU);
    fat_entry(bytes, 2, 0xffffU);
    fat_entry(bytes, 3, 7U);
    fat_entry(bytes, 7, 0xffffU);
    ascii(bytes, root_offset, "DEMOS      ");
    bytes[root_offset + 11U] = std::byte{0x10};
    le16(bytes, root_offset + 26U, 2U);
    ascii(bytes, data_offset, "DEMO1   S1A");
    bytes[data_offset + 11U] = std::byte{0x20};
    le16(bytes, data_offset + 26U, 3U);
    le32(bytes, data_offset + 28U, 700U);
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + sector_bytes), 512U, std::byte{0x31});
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + 5U * sector_bytes), 188U, std::byte{0x72});
    return bytes;
}

inline std::vector<std::byte> ex5_capacity_fixture(bool unused_tail = false, bool populated = false) {
    constexpr std::size_t cluster_bytes = 4U * sector_bytes;
    constexpr std::size_t table_bytes = 17U * sector_bytes;
    constexpr std::size_t root = sector_bytes + 2U * table_bytes;
    constexpr std::size_t data = root + 512U * 32U;
    const auto declared = data + 4096U * cluster_bytes + (unused_tail ? sector_bytes : 0U);
    std::vector<std::byte> bytes(declared - sector_bytes);
    ascii(bytes, 3U, "YAMAHA??");
    le16(bytes, 11U, 512U);
    bytes[13] = std::byte{4};
    le16(bytes, 14U, 1U);
    bytes[16] = std::byte{2};
    le16(bytes, 17U, 512U);
    bytes[21] = std::byte{0xf8};
    le16(bytes, 22U, 17U);
    le32(bytes, 32U, static_cast<std::uint32_t>(declared / sector_bytes));
    ascii(bytes, 54U, "FAT16   ");
    le16(bytes, 510U, 0xaa55U);
    for (const auto offset : {sector_bytes, sector_bytes + table_bytes}) {
        le16(bytes, offset, 0xfff8U);
        le16(bytes, offset + 2U, 0xffffU);
        if (populated) {
            le16(bytes, offset + 4U, 0xffffU);
            le16(bytes, offset + 6U, 0xffffU);
        }
    }
    if (populated) {
        ascii(bytes, root, "DEMOS      ");
        bytes[root + 11U] = std::byte{0x10};
        le16(bytes, root + 26U, 2U);
        ascii(bytes, data, "DEMO1   S1A");
        bytes[data + 11U] = std::byte{0x20};
        le16(bytes, data + 26U, 3U);
        le32(bytes, data + 28U, 700U);
        std::fill_n(bytes.begin() + data + cluster_bytes, 512U, std::byte{0x31});
        std::fill_n(bytes.begin() + data + cluster_bytes + 512U, 188U, std::byte{0x72});
    }
    return bytes;
}

} // namespace
