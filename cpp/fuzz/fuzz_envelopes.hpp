#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace axk::fuzz {

inline void le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

inline void le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4U; ++index)
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

inline void be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 8U);
  bytes[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

inline void be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4U; ++index)
    bytes[offset + index] = static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
}

inline std::vector<std::byte> fat_record_envelope(std::span<const std::byte> input) {
  constexpr std::size_t image_size = 1'474'560U;
  constexpr std::size_t root_offset = 19U * 512U;
  std::vector<std::byte> bytes(image_size);
  bytes[0] = std::byte{0xeb};
  bytes[1] = std::byte{0x3c};
  bytes[2] = std::byte{0x90};
  le16(bytes, 0x0b, 512U);
  bytes[0x0d] = std::byte{1};
  le16(bytes, 0x0e, 1U);
  bytes[0x10] = std::byte{2};
  le16(bytes, 0x11, 224U);
  le16(bytes, 0x13, 2880U);
  bytes[0x15] = std::byte{0xf0};
  le16(bytes, 0x16, 9U);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xaa};
  for (const auto fat_offset : {512U, 5120U}) {
    bytes[fat_offset] = std::byte{0xf0};
    bytes[fat_offset + 1U] = std::byte{0xff};
    bytes[fat_offset + 2U] = std::byte{0xff};
  }
  std::ranges::copy(input.first(std::min(input.size(), bytes.size() - root_offset)),
                    bytes.begin() + static_cast<std::ptrdiff_t>(root_offset));
  return bytes;
}

inline std::vector<std::byte> iso_record_envelope(std::span<const std::byte> input) {
  constexpr std::size_t sector_size = 2048U;
  constexpr std::size_t sector_count = 20U;
  std::vector<std::byte> bytes(sector_size * sector_count);
  const auto descriptor = 16U * sector_size;
  bytes[descriptor] = std::byte{1};
  for (std::size_t index = 0; index < 5U; ++index)
    bytes[descriptor + 1U + index] = static_cast<std::byte>("CD001"[index]);
  bytes[descriptor + 6U] = std::byte{1};
  le32(bytes, descriptor + 80U, sector_count);
  be32(bytes, descriptor + 84U, sector_count);
  le16(bytes, descriptor + 128U, sector_size);
  be16(bytes, descriptor + 130U, sector_size);
  const auto root_record = descriptor + 156U;
  bytes[root_record] = std::byte{34};
  le32(bytes, root_record + 2U, 18U);
  be32(bytes, root_record + 6U, 18U);
  le32(bytes, root_record + 10U, sector_size);
  be32(bytes, root_record + 14U, sector_size);
  bytes[root_record + 25U] = std::byte{2};
  bytes[root_record + 32U] = std::byte{1};
  const auto terminator = 17U * sector_size;
  bytes[terminator] = std::byte{255};
  for (std::size_t index = 0; index < 5U; ++index)
    bytes[terminator + 1U + index] = static_cast<std::byte>("CD001"[index]);
  bytes[terminator + 6U] = std::byte{1};
  const auto directory = 18U * sector_size;
  std::ranges::copy(input.first(std::min(input.size(), bytes.size() - directory)),
                    bytes.begin() + static_cast<std::ptrdiff_t>(directory));
  return bytes;
}

inline std::vector<std::byte> typed_object_envelope(std::span<const std::byte> input,
                                                    std::string_view object_type) {
  constexpr std::string_view magic{"FSFSDEV3SPLX"};
  std::vector<std::byte> payload(std::max<std::size_t>(0x400U, 0x40U + input.size()));
  std::ranges::copy(input, payload.begin() + 0x40);
  std::ranges::transform(magic, payload.begin(), [](char value) {
    return static_cast<std::byte>(value);
  });
  std::ranges::transform(object_type, payload.begin() + 0x0c, [](char value) {
    return static_cast<std::byte>(value);
  });
  return payload;
}

}  // namespace axk::fuzz
