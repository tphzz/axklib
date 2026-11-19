#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "axklib/error.hpp"

namespace axk::typhoon::iff {
using Bytes = std::span<const std::byte>;
inline Error invalid(std::string message) {
    return make_error(ErrorCode::object_malformed, ErrorCategory::object, std::move(message));
}
inline std::uint16_t u16(Bytes b, std::size_t n) {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(b[n]) << 8U) | std::to_integer<unsigned>(b[n + 1]));
}
inline std::uint32_t u32(Bytes b, std::size_t n) {
    return (static_cast<std::uint32_t>(u16(b, n)) << 16U) | u16(b, n + 2);
}
inline std::string_view ascii(Bytes b) { return {reinterpret_cast<const char *>(b.data()), b.size()}; }
inline Result<std::string_view> form(Bytes b) {
    if (b.size() < 12 || ascii(b.first(4)) != "FORM" || u32(b, 4) != b.size() - 8)
        return std::unexpected(invalid("Invalid IFF FORM extent"));
    return ascii(b.subspan(8, 4));
}
template <class Visitor> Result<void> chunks(Bytes b, Visitor visit) {
    while (!b.empty()) {
        if (b.size() < 8)
            return std::unexpected(invalid("Truncated IFF chunk header"));
        const auto size = u32(b, 4);
        const auto padded = static_cast<std::uint64_t>(size) + (size & 1U);
        if (padded > b.size() - 8)
            return std::unexpected(invalid("Truncated IFF chunk"));
        if (auto ok = visit(ascii(b.first(4)), b.subspan(8, size)); !ok)
            return ok;
        b = b.subspan(8 + static_cast<std::size_t>(padded));
    }
    return {};
}
} // namespace axk::typhoon::iff
