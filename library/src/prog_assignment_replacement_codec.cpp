#include "alteration_manifest_program.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/prog_codec.hpp"
#include "axklib/program_parameter_codec.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::detail {

Result<std::vector<std::byte>> replace_prog_assignment_rows(std::span<const std::byte> payload,
                                                            const ReplaceProgramAssignmentsOperation &operation) {
    if (auto valid = validate_program_assignment_replacement(operation); !valid)
        return std::unexpected{valid.error()};
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *program = std::get_if<CurrentProg>(&decoded->payload);
    if (!program || !program->layout.parameter_tail_offset)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Assignment replacement requires a complete current Program")};
    const auto &layout = program->layout;
    ProgramSpec spec;
    spec.number = operation.program_number;
    spec.name = "Rows";
    spec.model = operation.model;
    auto neutral = prepare_prog_payload(spec);
    if (!neutral)
        return std::unexpected{neutral.error()};
    for (const auto &edit : operation.assignments) {
        if (edit.retain_ordinal && *edit.retain_ordinal >= program->assignments.size())
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "Retained assignment ordinal is outside the stored count")};
        spec.assignments.push_back(edit.assignment.value_or(ProgramAssignmentSpec{"SBNK", "Reserved", {}}));
    }
    auto fresh = prepare_prog_payload(spec);
    if (!fresh)
        return std::unexpected{fresh.error()};
    const auto capacity = std::max({layout.assignment_capacity, std::size_t{8}, operation.assignments.size()});
    const auto growth = (capacity - layout.assignment_capacity) * prog_assignment_stride;
    const auto old_tail = *layout.parameter_tail_offset;
    std::vector<std::byte> result(payload.begin(), payload.end());
    result.insert(result.begin() + static_cast<std::ptrdiff_t>(old_tail), growth, std::byte{});
    const auto neutral_row = std::span{*neutral}.subspan(prog_assignment_start, prog_assignment_stride);
    for (std::size_t index = 0; index < capacity; ++index) {
        const auto offset = prog_assignment_start + index * prog_assignment_stride;
        auto output = std::span{result}.subspan(offset, prog_assignment_stride);
        if (index >= operation.assignments.size()) {
            if (index < program->assignments.size() || index >= layout.assignment_capacity)
                std::ranges::copy(neutral_row, output.begin());
            continue;
        }
        const auto &edit = operation.assignments[index];
        if (!edit.retain_ordinal) {
            std::ranges::copy(std::span{*fresh}.subspan(offset, prog_assignment_stride), output.begin());
            continue;
        }
        const auto &retained = program->assignments[*edit.retain_ordinal];
        std::ranges::copy(retained.raw_row, output.begin());
        if (edit.assignment) {
            const auto &target = *edit.assignment;
            const auto kind = target.target_kind == "SBAC" ? std::byte{0x11} : std::byte{0x10};
            if (retained.name != target.target_name || static_cast<std::byte>(retained.kind) != kind) {
                ByteWriter writer{output};
                if (auto written = writer.write_ascii_field(0U, 16U, target.target_name); !written)
                    return std::unexpected{written.error()};
                std::ranges::fill(output.subspan(0x10U, 4U), std::byte{});
                output[0x14U] = kind;
            }
        }
    }
    ByteWriter writer{result};
    for (const auto &[offset, value] : {std::pair{std::size_t{0x18}, layout.logical_size + growth - 0xe0U},
                                        std::pair{std::size_t{0x1c}, layout.logical_size + growth - 0x30U}}) {
        if (auto written = writer.write_be32(offset, static_cast<std::uint32_t>(value)); !written)
            return std::unexpected{written.error()};
    }
    if (auto written = writer.write_be16(0x96U, static_cast<std::uint16_t>(operation.assignments.size())); !written)
        return std::unexpected{written.error()};
    std::vector<ProgramAssignmentParameterPatch> patches;
    for (std::size_t index = 0; index < operation.assignments.size(); ++index) {
        const auto &edit = operation.assignments[index];
        if (edit.retain_ordinal && edit.assignment &&
            has_program_assignment_parameter_values(edit.assignment->parameters))
            patches.push_back(
                {index, edit.assignment->target_kind, edit.assignment->target_name, edit.assignment->parameters});
    }
    if (auto applied = apply_program_assignment_patches(result, patches, operation.model); !applied)
        return std::unexpected{applied.error()};
    return result;
}

} // namespace axk::detail
