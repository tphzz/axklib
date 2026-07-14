#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "axklib/error.hpp"
#include "axklib/export.hpp"

namespace axk {

AXK_API Result<std::uint64_t> checked_add(std::uint64_t left,
                                          std::uint64_t right);
AXK_API Result<std::uint64_t> checked_subtract(std::uint64_t left,
                                               std::uint64_t right);
AXK_API Result<std::uint64_t> checked_multiply(std::uint64_t left,
                                               std::uint64_t right);
AXK_API Result<std::uint64_t> align_up(std::uint64_t value,
                                       std::uint64_t alignment);

class AXK_API ByteReader {
  public:
    explicit ByteReader(std::span<const std::byte> bytes) noexcept
        : bytes_{bytes} {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] Result<std::span<const std::byte>>
    slice(std::size_t offset, std::size_t count) const;
    [[nodiscard]] Result<std::uint8_t> u8(std::size_t offset) const;
    [[nodiscard]] Result<std::int8_t> s8(std::size_t offset) const;
    [[nodiscard]] Result<std::uint16_t> be16(std::size_t offset) const;
    [[nodiscard]] Result<std::uint32_t> be32(std::size_t offset) const;
    [[nodiscard]] Result<std::int16_t> sbe16(std::size_t offset) const;
    [[nodiscard]] Result<std::int32_t> sbe32(std::size_t offset) const;
    [[nodiscard]] Result<std::uint16_t> le16(std::size_t offset) const;
    [[nodiscard]] Result<std::uint32_t> le32(std::size_t offset) const;
    [[nodiscard]] Result<std::int16_t> sle16(std::size_t offset) const;
    [[nodiscard]] Result<std::int32_t> sle32(std::size_t offset) const;
    [[nodiscard]] Result<std::string>
    ascii_field(std::size_t offset, std::size_t count,
                bool replace_invalid = true) const;
    [[nodiscard]] Result<std::string>
    printable_ascii_field(std::size_t offset, std::size_t count) const;
    [[nodiscard]] Result<std::string>
    decoded_ascii_field(std::size_t offset, std::size_t count) const;

  private:
    std::span<const std::byte> bytes_;
};

class AXK_API ByteWriter {
  public:
    explicit ByteWriter(std::span<std::byte> bytes) noexcept : bytes_{bytes} {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] Result<void> write_u8(std::size_t offset, std::uint8_t value);
    [[nodiscard]] Result<void> write_be16(std::size_t offset,
                                          std::uint16_t value);
    [[nodiscard]] Result<void> write_be32(std::size_t offset,
                                          std::uint32_t value);
    [[nodiscard]] Result<void> write_le16(std::size_t offset,
                                          std::uint16_t value);
    [[nodiscard]] Result<void> write_le32(std::size_t offset,
                                          std::uint32_t value);
    [[nodiscard]] Result<void>
    write_ascii_field(std::size_t offset, std::size_t count,
                      std::string_view value, std::byte pad = std::byte{' '});

  private:
    [[nodiscard]] Result<std::span<std::byte>> slice(std::size_t offset,
                                                     std::size_t count);

    std::span<std::byte> bytes_;
};

} // namespace axk
