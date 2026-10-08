#include "alteration_manifest_program.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/prog_codec.hpp"
#include "axklib/program_format_conversion.hpp"
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
    const bool native = operation.model == ASeriesModel::a3000;
    if (!program || program->layout.version != (native ? 2U : 4U))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Assignment replacement model must match revision-2 or revision-4 storage")};
    const auto &layout = program->layout;
    ProgramSpec spec;
    spec.number = operation.program_number;
    spec.name = "Rows";
    auto neutral = prepare_prog_payload(spec);
    if (!neutral)
        return std::unexpected{neutral.error()};
    // Only the empty template is converted. Retained Program bytes never pass through conversion.
    if (native) {
        auto converted = plan_program_format_conversion(*neutral, ProgramStorageFormat::a3000);
        if (!converted.allowed())
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "Native assignment defaults are unavailable")};
        neutral = std::move(converted.converted_payload);
    }
    for (const auto &edit : operation.assignments) {
        if (edit.retain_ordinal && *edit.retain_ordinal >= program->assignments.size())
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "Retained assignment ordinal is outside the stored count")};
    }
    const auto capacity = std::max({layout.assignment_capacity, std::size_t{8}, operation.assignments.size()});
    const auto growth = (capacity - layout.assignment_capacity) * prog_assignment_stride;
    const auto old_tail = layout.parameter_tail_offset.value_or(layout.logical_size);
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
            std::ranges::copy(neutral_row, output.begin());
        } else {
            const auto &retained = program->assignments[*edit.retain_ordinal];
            std::ranges::copy(retained.raw_row, output.begin());
        }
        if (edit.assignment) {
            const auto &target = *edit.assignment;
            const auto kind = target.target_kind == "SBAC" ? std::byte{0x11} : std::byte{0x10};
            const auto *retained = edit.retain_ordinal ? &program->assignments[*edit.retain_ordinal] : nullptr;
            if (!retained || retained->name != target.target_name || static_cast<std::byte>(retained->kind) != kind) {
                ByteWriter writer{output};
                if (auto written = writer.write_ascii_field(0U, 16U, target.target_name); !written)
                    return std::unexpected{written.error()};
                std::ranges::fill(output.subspan(0x10U, 4U), std::byte{});
                output[0x14U] = kind;
            }
        }
    }
    ByteWriter writer{result};
    for (const auto &[offset, value] :
         {std::pair{std::size_t{0x18}, layout.logical_size + growth - (native ? 0x30U : 0xe0U)},
          std::pair{std::size_t{0x1c}, layout.logical_size + growth - 0x30U}}) {
        if (native && offset == 0x1cU)
            continue;
        if (auto written = writer.write_be32(offset, static_cast<std::uint32_t>(value)); !written)
            return std::unexpected{written.error()};
    }
    if (auto written = writer.write_be16(0x96U, static_cast<std::uint16_t>(operation.assignments.size())); !written)
        return std::unexpected{written.error()};
    std::vector<ProgramAssignmentParameterPatch> patches;
    for (std::size_t index = 0; index < operation.assignments.size(); ++index) {
        const auto &edit = operation.assignments[index];
        if (edit.assignment && has_program_assignment_parameter_values(edit.assignment->parameters))
            patches.push_back(
                {index, edit.assignment->target_kind, edit.assignment->target_name, edit.assignment->parameters});
    }
    if (auto applied = apply_program_assignment_patches(result, patches, operation.model); !applied)
        return std::unexpected{applied.error()};
    if (auto applied = apply_program_parameters(result, operation.parameters, operation.model); !applied)
        return std::unexpected{applied.error()};
    return result;
}

} // namespace axk::detail
