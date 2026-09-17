#include "axklib/prog_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/program_parameter_codec.hpp"

namespace axk::detail {

Result<ProgLayout> decode_prog_layout(std::span<const std::byte> payload, const ObjectHeader &header) {
    const auto version = header.unknown_0x14;
    if (version != 1U && version != 2U && version != 4U) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "unsupported Program layout selector")};
    }
    const auto current = version == 4U;
    const auto length = checked_add(header.record_size_or_header_used, current ? 0xe0U : 0x30U);
    if (!length)
        return std::unexpected{length.error()};
    if (current && *length != static_cast<std::uint64_t>(header.payload_bytes_0x1c) + 0x30U) {
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::object, "Program header length views disagree")};
    }
    const auto tail_size = current ? prog_parameter_tail_size : 0U;
    if (*length < prog_assignment_start + tail_size) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Program length does not include its fixed parameter blocks")};
    }
    if (*length > payload.size()) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "Program logical extent exceeds its container payload")};
    }
    const auto logical_size = static_cast<std::size_t>(*length);
    const auto row_bytes = logical_size - tail_size - prog_assignment_start;
    const auto count = ByteReader{payload}.be16(0x96U);
    if (!count)
        return std::unexpected{count.error()};
    if (row_bytes % prog_assignment_stride != 0U || *count > maximum_stored_program_assignments ||
        *count > row_bytes / prog_assignment_stride) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Program assignment count or row capacity is invalid")};
    }
    return ProgLayout{current ? ProgStorageLayout::current_split_parameter_tail
                              : ProgStorageLayout::legacy_without_parameter_tail,
                      version,
                      logical_size,
                      *count,
                      row_bytes / prog_assignment_stride,
                      current ? std::optional<std::size_t>{logical_size - tail_size} : std::nullopt};
}

Result<std::size_t> prog_assignment_offset(const ProgLayout &layout, std::size_t ordinal) {
    if (ordinal >= layout.stored_assignment_count || ordinal >= layout.assignment_capacity) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Program assignment ordinal is outside its stored count")};
    }
    return prog_assignment_start + ordinal * prog_assignment_stride;
}

Result<CurrentProg> decode_prog(std::span<const std::byte> payload, const ObjectHeader &header,
                                CurrentObjectCommonRecord common) {
    const auto layout = decode_prog_layout(payload, header);
    if (!layout)
        return std::unexpected{layout.error()};
    const ByteReader reader{payload.first(layout->logical_size)};
    CurrentProg result;
    result.common = std::move(common);
    result.layout = *layout;
    result.program_name = *reader.decoded_ascii_field(0x78U, 8U);
    result.parameters = decode_program_parameters(payload, *layout);
    std::ranges::copy(*reader.slice(0x80U, 0x18U), result.raw_common_parameter_block.begin());
    if (layout->parameter_tail_offset) {
        const auto extended = *reader.slice(*layout->parameter_tail_offset + 0x88U, 0x28U);
        result.raw_extended_parameter_block.assign(extended.begin(), extended.end());
    }
    const auto controls = layout->parameter_tail_offset ? *layout->parameter_tail_offset + 0x78U : 0x110U;
    std::ranges::copy(*reader.slice(controls, 16U), result.raw_canonical_control_block.begin());
    std::ranges::copy(*reader.slice(0x110U, 16U), result.raw_legacy_control_block.begin());
    const auto effects = layout->parameter_tail_offset ? 6U : 3U;
    for (std::size_t index = 0; index < effects; ++index) {
        const auto offset = index < 3U ? 0x98U + index * 0x28U : *layout->parameter_tail_offset + (index - 3U) * 0x28U;
        ProgEffectBlock effect;
        std::ranges::copy(*reader.slice(offset, 0x28U), effect.raw_bytes.begin());
        effect.type = *reader.u8(offset + (layout->parameter_tail_offset ? 6U : 7U));
        if (layout->version == 1U) {
            if (effect.type >= 47U && effect.type <= 51U)
                effect.type = 0U;
            else if (effect.type >= 52U)
                effect.type = static_cast<std::uint16_t>(effect.type - 5U);
        }
        for (std::size_t parameter = 0; parameter < effect.parameter_values.size(); ++parameter)
            effect.parameter_values[parameter] = *reader.be16(offset + 8U + parameter * 2U);
        result.parameters.effects[index] = decode_program_effect_parameters(effect, index);
        result.effect_blocks.push_back(effect);
    }
    result.assignments.reserve(layout->stored_assignment_count);
    for (std::size_t index = 0; index < layout->stored_assignment_count; ++index) {
        const auto offset = prog_assignment_offset(*layout, index);
        if (!offset)
            return std::unexpected{offset.error()};
        ProgAssignment row;
        row.parameters = decode_program_assignment_parameters(*reader.slice(*offset, prog_assignment_stride),
                                                              layout->storage_layout);
        row.offset = *offset;
        row.name = payload[*offset] == std::byte{} ? std::string{} : *reader.decoded_ascii_field(*offset, 16U);
        row.raw_handle = *reader.be32(*offset + 0x10U);
        row.kind = *reader.u8(*offset + 0x14U);
        row.raw_receive_selector = *reader.u8(*offset + 0x15U);
        std::ranges::copy(*reader.slice(*offset, prog_assignment_stride), row.raw_row.begin());
        result.assignments.push_back(std::move(row));
    }
    return result;
}

} // namespace axk::detail
