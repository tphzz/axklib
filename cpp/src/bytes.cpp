#include "axklib/bytes.hpp"

#include <algorithm>
#include <limits>

namespace axk {
namespace {

Error bounds_error(std::size_t offset, std::size_t count, std::size_t size) {
  ErrorContext context;
  context.raw_offset = offset;
  return make_error(
      ErrorCode::out_of_bounds,
      ErrorCategory::container,
      "byte range at offset " + std::to_string(offset) + " with length " +
          std::to_string(count) + " exceeds buffer size " + std::to_string(size),
      std::move(context));
}

}  // namespace

Result<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::unexpected{make_error(
        ErrorCode::integer_overflow,
        ErrorCategory::container,
        "unsigned 64-bit addition overflow")};
  }
  return left + right;
}

Result<std::uint64_t> checked_subtract(std::uint64_t left, std::uint64_t right) {
  if (right > left) {
    return std::unexpected{make_error(
        ErrorCode::integer_overflow,
        ErrorCategory::container,
        "unsigned 64-bit subtraction underflow")};
  }
  return left - right;
}

Result<std::uint64_t> checked_multiply(std::uint64_t left, std::uint64_t right) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::unexpected{make_error(
        ErrorCode::integer_overflow,
        ErrorCategory::container,
        "unsigned 64-bit multiplication overflow")};
  }
  return left * right;
}

Result<std::uint64_t> align_up(std::uint64_t value, std::uint64_t alignment) {
  if (alignment == 0) {
    return std::unexpected{make_error(
        ErrorCode::invalid_argument,
        ErrorCategory::container,
        "alignment must be nonzero")};
  }
  const auto remainder = value % alignment;
  return remainder == 0 ? Result<std::uint64_t>{value}
                        : checked_add(value, alignment - remainder);
}

Result<std::span<const std::byte>> ByteReader::slice(
    std::size_t offset,
    std::size_t count) const {
  if (offset > bytes_.size() || count > bytes_.size() - offset) {
    return std::unexpected{bounds_error(offset, count, bytes_.size())};
  }
  return bytes_.subspan(offset, count);
}

Result<std::uint8_t> ByteReader::u8(std::size_t offset) const {
  const auto data = slice(offset, 1);
  if (!data) {
    return std::unexpected{data.error()};
  }
  return std::to_integer<std::uint8_t>((*data)[0]);
}

Result<std::int8_t> ByteReader::s8(std::size_t offset) const {
  const auto value = u8(offset);
  if (!value) {
    return std::unexpected{value.error()};
  }
  return static_cast<std::int8_t>(*value);
}

Result<std::uint16_t> ByteReader::be16(std::size_t offset) const {
  const auto data = slice(offset, 2);
  if (!data) {
    return std::unexpected{data.error()};
  }
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*data)[0]) << 8U) |
         std::to_integer<std::uint8_t>((*data)[1]);
}

Result<std::uint32_t> ByteReader::be32(std::size_t offset) const {
  const auto data = slice(offset, 4);
  if (!data) {
    return std::unexpected{data.error()};
  }
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[3]));
}

Result<std::int16_t> ByteReader::sbe16(std::size_t offset) const {
  const auto value = be16(offset);
  return value ? Result<std::int16_t>{static_cast<std::int16_t>(*value)}
               : std::unexpected{value.error()};
}

Result<std::int32_t> ByteReader::sbe32(std::size_t offset) const {
  const auto value = be32(offset);
  return value ? Result<std::int32_t>{static_cast<std::int32_t>(*value)}
               : std::unexpected{value.error()};
}

Result<std::uint16_t> ByteReader::le16(std::size_t offset) const {
  const auto data = slice(offset, 2);
  if (!data) {
    return std::unexpected{data.error()};
  }
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*data)[0])) |
         static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*data)[1]) << 8U);
}

Result<std::uint32_t> ByteReader::le32(std::size_t offset) const {
  const auto data = slice(offset, 4);
  if (!data) {
    return std::unexpected{data.error()};
  }
  return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[0])) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[1])) << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[2])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[3])) << 24U);
}

Result<std::int16_t> ByteReader::sle16(std::size_t offset) const {
  const auto value = le16(offset);
  return value ? Result<std::int16_t>{static_cast<std::int16_t>(*value)}
               : std::unexpected{value.error()};
}

Result<std::int32_t> ByteReader::sle32(std::size_t offset) const {
  const auto value = le32(offset);
  return value ? Result<std::int32_t>{static_cast<std::int32_t>(*value)}
               : std::unexpected{value.error()};
}

Result<std::string> ByteReader::ascii_field(
    std::size_t offset,
    std::size_t count,
    bool replace_invalid) const {
  const auto data = slice(offset, count);
  if (!data) {
    return std::unexpected{data.error()};
  }
  std::string result;
  result.reserve(count);
  for (const auto value : *data) {
    const auto character = std::to_integer<std::uint8_t>(value);
    if (character == 0) {
      break;
    }
    if (character > 0x7fU) {
      if (!replace_invalid) {
        ErrorContext context;
        context.raw_offset = offset + result.size();
        return std::unexpected{make_error(
            ErrorCode::invalid_ascii,
            ErrorCategory::object,
            "fixed field contains non-ASCII byte",
            std::move(context))};
      }
      result.push_back('?');
    } else {
      result.push_back(static_cast<char>(character));
    }
  }
  while (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

Result<std::string> ByteReader::printable_ascii_field(
    std::size_t offset,
    std::size_t count) const {
  const auto data = slice(offset, count);
  if (!data) {
    return std::unexpected{data.error()};
  }
  std::size_t end = data->size();
  while (end > 0) {
    const auto value = std::to_integer<std::uint8_t>((*data)[end - 1U]);
    if (value != 0 && value != 0x20U) {
      break;
    }
    --end;
  }
  std::string result;
  result.reserve(end);
  for (const auto value : data->first(end)) {
    const auto character = std::to_integer<std::uint8_t>(value);
    result.push_back(
        character >= 0x20U && character < 0x7fU ? static_cast<char>(character) : '?');
  }
  return result;
}

Result<std::string> ByteReader::decoded_ascii_field(
    std::size_t offset,
    std::size_t count) const {
  const auto data = slice(offset, count);
  if (!data) return std::unexpected{data.error()};
  std::size_t end = data->size();
  while (end > 0) {
    const auto value = std::to_integer<std::uint8_t>((*data)[end - 1U]);
    if (value != 0 && value != 0x20U) break;
    --end;
  }
  std::string result;
  for (const auto value : data->first(end)) {
    const auto character = std::to_integer<std::uint8_t>(value);
    if (character < 0x80U) {
      result.push_back(static_cast<char>(character));
    } else {
      result += "\xef\xbf\xbd";
    }
  }
  return result;
}

Result<std::span<std::byte>> ByteWriter::slice(std::size_t offset, std::size_t count) {
  if (offset > bytes_.size() || count > bytes_.size() - offset) {
    return std::unexpected{bounds_error(offset, count, bytes_.size())};
  }
  return bytes_.subspan(offset, count);
}

Result<void> ByteWriter::write_u8(std::size_t offset, std::uint8_t value) {
  const auto data = slice(offset, 1);
  if (!data) {
    return std::unexpected{data.error()};
  }
  (*data)[0] = static_cast<std::byte>(value);
  return {};
}

Result<void> ByteWriter::write_be16(std::size_t offset, std::uint16_t value) {
  const auto data = slice(offset, 2);
  if (!data) {
    return std::unexpected{data.error()};
  }
  (*data)[0] = static_cast<std::byte>(value >> 8U);
  (*data)[1] = static_cast<std::byte>(value);
  return {};
}

Result<void> ByteWriter::write_be32(std::size_t offset, std::uint32_t value) {
  const auto data = slice(offset, 4);
  if (!data) {
    return std::unexpected{data.error()};
  }
  (*data)[0] = static_cast<std::byte>(value >> 24U);
  (*data)[1] = static_cast<std::byte>(value >> 16U);
  (*data)[2] = static_cast<std::byte>(value >> 8U);
  (*data)[3] = static_cast<std::byte>(value);
  return {};
}

Result<void> ByteWriter::write_le16(std::size_t offset, std::uint16_t value) {
  const auto data = slice(offset, 2);
  if (!data) {
    return std::unexpected{data.error()};
  }
  (*data)[0] = static_cast<std::byte>(value);
  (*data)[1] = static_cast<std::byte>(value >> 8U);
  return {};
}

Result<void> ByteWriter::write_le32(std::size_t offset, std::uint32_t value) {
  const auto data = slice(offset, 4);
  if (!data) {
    return std::unexpected{data.error()};
  }
  (*data)[0] = static_cast<std::byte>(value);
  (*data)[1] = static_cast<std::byte>(value >> 8U);
  (*data)[2] = static_cast<std::byte>(value >> 16U);
  (*data)[3] = static_cast<std::byte>(value >> 24U);
  return {};
}

Result<void> ByteWriter::write_ascii_field(
    std::size_t offset,
    std::size_t count,
    std::string_view value,
    std::byte pad) {
  if (value.size() > count) {
    return std::unexpected{make_error(
        ErrorCode::invalid_argument,
        ErrorCategory::object,
        "ASCII value does not fit fixed field")};
  }
  if (std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character > 0x7fU;
      })) {
    return std::unexpected{make_error(
        ErrorCode::invalid_ascii,
        ErrorCategory::object,
        "fixed field value must contain ASCII text")};
  }
  const auto data = slice(offset, count);
  if (!data) {
    return std::unexpected{data.error()};
  }
  std::fill(data->begin(), data->end(), pad);
  std::transform(value.begin(), value.end(), data->begin(), [](char character) {
    return static_cast<std::byte>(static_cast<unsigned char>(character));
  });
  return {};
}

}  // namespace axk
