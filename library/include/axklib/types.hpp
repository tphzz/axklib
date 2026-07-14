#pragma once

#include <compare>
#include <cstdint>

#include "axklib/platform.hpp"

namespace axk {

struct PartitionIndex {
    std::uint8_t value{};
    friend auto operator<=>(const PartitionIndex &, const PartitionIndex &) = default;
};

struct SfsId {
    std::uint32_t value{};
    friend auto operator<=>(const SfsId &, const SfsId &) = default;
};

struct LinkId {
    std::uint32_t value{};
    friend auto operator<=>(const LinkId &, const LinkId &) = default;
};

struct ByteOffset {
    std::uint64_t value{};
    friend auto operator<=>(const ByteOffset &, const ByteOffset &) = default;
};

} // namespace axk
