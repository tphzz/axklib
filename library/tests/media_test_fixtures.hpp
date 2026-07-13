#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/media.hpp"
#include "axklib/semantic.hpp"

namespace {

inline void le16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
}

inline void le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
  le16(bytes, offset, static_cast<std::uint16_t>(value));
  le16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16U));
}

inline void be16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 8U);
  bytes[offset + 1] = static_cast<std::byte>(value & 0xffU);
}

inline void be32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
  be16(bytes, offset, static_cast<std::uint16_t>(value >> 16U));
  be16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

inline void ascii(std::span<std::byte> bytes, std::size_t offset, std::string_view value) {
  std::ranges::transform(value, bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         [](char ch) { return static_cast<std::byte>(ch); });
}

inline std::vector<std::byte> smpl_object(std::string_view name = "TEST") {
  std::vector<std::byte> bytes(0xb0);
  ascii(bytes, 0, "FSFSDEV3SPLX");
  ascii(bytes, 0x0c, "SMPL");
  be32(bytes, 0x10, 0xac);
  be32(bytes, 0x1c, 4);
  be32(bytes, 0x20, 4);
  be16(bytes, 0x28, 32000);
  be16(bytes, 0x2a, 2);
  ascii(bytes, 0x32, name);
  be16(bytes, 0x8c, 32000);
  be32(bytes, 0x96, 2);
  be32(bytes, 0x9e, 2);
  bytes[0xac] = std::byte{0x12};
  bytes[0xad] = std::byte{0x34};
  bytes[0xae] = std::byte{0x56};
  bytes[0xaf] = std::byte{0x78};
  return bytes;
}

inline void set_fat12(std::span<std::byte> fat, std::uint16_t cluster, std::uint16_t value) {
  const auto offset = static_cast<std::size_t>(cluster) + cluster / 2U;
  auto pair = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset])) |
              static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset + 1])) << 8U;
  auto wide_pair = static_cast<std::uint32_t>(pair);
  const auto wide_value = static_cast<std::uint32_t>(value);
  if ((cluster & 1U) != 0U)
    wide_pair = (wide_pair & 0x000fU) | (wide_value << 4U);
  else
    wide_pair = (wide_pair & 0xf000U) | (wide_value & 0x0fffU);
  pair = static_cast<std::uint16_t>(wide_pair);
  fat[offset] = static_cast<std::byte>(wide_pair & 0xffU);
  fat[offset + 1] = static_cast<std::byte>(pair >> 8U);
}

inline std::vector<std::byte> fat_fixture(std::uint16_t chain_end = 0xfffU) {
  constexpr std::size_t sectors = 100;
  constexpr std::size_t sector_size = 512;
  constexpr std::size_t root_offset = 3 * sector_size;
  constexpr std::size_t data_offset = 4 * sector_size;
  std::vector<std::byte> bytes(sectors * sector_size);
  le16(bytes, 0x0b, sector_size);
  bytes[0x0d] = std::byte{1};
  le16(bytes, 0x0e, 1);
  bytes[0x10] = std::byte{2};
  le16(bytes, 0x11, 16);
  le16(bytes, 0x13, sectors);
  bytes[0x15] = std::byte{0xf0};
  le16(bytes, 0x16, 1);
  for (const auto fat_offset : {sector_size, 2 * sector_size}) {
    bytes[fat_offset] = std::byte{0xf0};
    bytes[fat_offset + 1] = std::byte{0xff};
    bytes[fat_offset + 2] = std::byte{0xff};
    set_fat12(std::span{bytes}.subspan(fat_offset, sector_size), 2, chain_end);
  }
  ascii(bytes, root_offset, "SMPTEST ");
  ascii(bytes, root_offset + 8, "004");
  bytes[root_offset + 0x0b] = std::byte{0x20};
  le16(bytes, root_offset + 0x1a, 2);
  const auto object = smpl_object();
  le32(bytes, root_offset + 0x1c, static_cast<std::uint32_t>(object.size()));
  std::ranges::copy(object, bytes.begin() + data_offset);
  return bytes;
}

inline std::vector<std::byte> nested_fat_fixture() {
  auto bytes = fat_fixture();
  const auto object = smpl_object();
  constexpr std::size_t root = 3U * 512U;
  constexpr std::size_t data = 4U * 512U;
  std::fill_n(bytes.begin() + root, 32, std::byte{});
  ascii(bytes, root, "OBJECTS ");
  bytes[root + 0x0b] = std::byte{0x10};
  le16(bytes, root + 0x1a, 2);
  std::fill_n(bytes.begin() + data, 512, std::byte{});
  ascii(bytes, data, "SMPTEST ");
  ascii(bytes, data + 8, "004");
  bytes[data + 0x0b] = std::byte{0x20};
  le16(bytes, data + 0x1a, 3);
  le32(bytes, data + 0x1c, static_cast<std::uint32_t>(object.size()));
  std::ranges::copy(object, bytes.begin() + data + 512);
  for (const auto fat_offset : {512U, 1024U}) {
    auto fat = std::span{bytes}.subspan(fat_offset, 512);
    set_fat12(fat, 2, 0xfff);
    set_fat12(fat, 3, 0xfff);
  }
  return bytes;
}

inline std::vector<std::byte> fat_fixture_with_invalid_then_valid_waveform() {
  auto bytes = fat_fixture();
  constexpr std::size_t root = 3U * 512U;
  constexpr std::size_t data = 4U * 512U;
  const auto valid = smpl_object("VALID");
  be32(bytes, data + 0x10U, 0xafU);
  ascii(bytes, root + 32U, "SMPVALID");
  ascii(bytes, root + 40U, "004");
  bytes[root + 32U + 0x0bU] = std::byte{0x20};
  le16(bytes, root + 32U + 0x1aU, 3);
  le32(bytes, root + 32U + 0x1cU, static_cast<std::uint32_t>(valid.size()));
  std::ranges::copy(valid, bytes.begin() + data + 512U);
  for (const auto fat_offset : {512U, 1024U})
    set_fat12(std::span{bytes}.subspan(fat_offset, 512), 3, 0xfff);
  return bytes;
}

inline std::vector<std::byte> iso_record(std::span<const std::byte> name, std::uint32_t extent,
                                         std::uint32_t size, std::uint8_t flags) {
  auto length = 33U + name.size();
  if ((length & 1U) != 0U)
    ++length;
  std::vector<std::byte> record(length);
  record[0] = static_cast<std::byte>(length);
  le32(record, 2, extent);
  be32(record, 6, extent);
  le32(record, 10, size);
  be32(record, 14, size);
  record[25] = static_cast<std::byte>(flags);
  le16(record, 28, 1);
  be16(record, 30, 1);
  record[32] = static_cast<std::byte>(name.size());
  std::ranges::copy(name, record.begin() + 33);
  return record;
}

inline std::vector<std::byte> iso_record(std::string_view name, std::uint32_t extent,
                                         std::uint32_t size, std::uint8_t flags) {
  std::vector<std::byte> bytes;
  bytes.reserve(name.size());
  for (const auto ch : name)
    bytes.push_back(static_cast<std::byte>(ch));
  return iso_record(bytes, extent, size, flags);
}

inline void append_record(std::span<std::byte> sector, std::size_t &offset,
                          const std::vector<std::byte> &record) {
  std::ranges::copy(record, sector.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += record.size();
}

inline std::vector<std::byte> iso_fixture(bool outside_extent = false) {
  constexpr std::size_t sector_size = 2048;
  constexpr std::uint32_t sector_count = 24;
  std::vector<std::byte> bytes(sector_count * sector_size);
  auto pvd = std::span{bytes}.subspan(16 * sector_size, sector_size);
  pvd[0] = std::byte{1};
  ascii(pvd, 1, "CD001");
  pvd[6] = std::byte{1};
  ascii(pvd, 40, "TESTVOL");
  le32(pvd, 80, sector_count);
  be32(pvd, 84, sector_count);
  le16(pvd, 128, sector_size);
  be16(pvd, 130, sector_size);
  const std::array dot{std::byte{0}};
  const std::array dotdot{std::byte{1}};
  const auto root_record = iso_record(dot, 18, sector_size, 2);
  std::ranges::copy(root_record, pvd.begin() + 156);

  auto root = std::span{bytes}.subspan(18 * sector_size, sector_size);
  std::size_t offset{};
  append_record(root, offset, root_record);
  append_record(root, offset, iso_record(dotdot, 18, sector_size, 2));
  append_record(root, offset, iso_record("GROUP", 19, sector_size, 2));

  auto group = std::span{bytes}.subspan(19 * sector_size, sector_size);
  offset = 0;
  append_record(group, offset, iso_record(dot, 19, sector_size, 2));
  append_record(group, offset, iso_record(dotdot, 18, sector_size, 2));
  append_record(group, offset, iso_record("F001", 20, sector_size, 2));
  append_record(group, offset, iso_record("0000.;1", 22, 32, 0));

  const auto object = smpl_object("CD WAVE");
  auto volume = std::span{bytes}.subspan(20 * sector_size, sector_size);
  offset = 0;
  append_record(volume, offset, iso_record(dot, 20, sector_size, 2));
  append_record(volume, offset, iso_record(dotdot, 19, sector_size, 2));
  append_record(volume, offset,
                iso_record("F000.;1", outside_extent ? 99 : 21,
                           static_cast<std::uint32_t>(object.size()), 0));
  if (!outside_extent)
    std::ranges::copy(object, bytes.begin() + 21 * sector_size);
  auto table = std::span{bytes}.subspan(22 * sector_size, 32);
  table[0] = std::byte{0xdd};
  ascii(table, 1, "  Mapped Vol");
  ascii(table, 18, "F001");
  return bytes;
}

} // namespace
