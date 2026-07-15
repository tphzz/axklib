#pragma once

#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::byte) == 1);
static_assert(sizeof(std::uint8_t) == 1);
static_assert(sizeof(std::uint16_t) == 2);
static_assert(sizeof(std::uint32_t) == 4);
static_assert(sizeof(std::uint64_t) == 8);
static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);

namespace axk::platform_contract {

using ExpectedFeature = std::expected<int, int>;
using SpanFeature = std::span<const std::byte>;
using PathFeature = std::filesystem::path;

} // namespace axk::platform_contract
