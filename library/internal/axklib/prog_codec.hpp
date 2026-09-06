#pragma once

#include <cstddef>
#include <span>

#include "axklib/object.hpp"

namespace axk::detail {

inline constexpr std::size_t prog_assignment_start = 0x120U;
inline constexpr std::size_t prog_assignment_stride = 0x38U;
inline constexpr std::size_t prog_parameter_tail_size = 0xb0U;

Result<ProgLayout> decode_prog_layout(std::span<const std::byte> payload, const ObjectHeader &header);
Result<std::size_t> prog_assignment_offset(const ProgLayout &layout, std::size_t ordinal);
Result<CurrentProg> decode_prog(std::span<const std::byte> payload, const ObjectHeader &header,
                                CurrentObjectCommonRecord common);

} // namespace axk::detail
