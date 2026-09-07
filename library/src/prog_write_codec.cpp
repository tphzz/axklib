#include "axklib/prog_codec.hpp"
#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/program_parameter_codec.hpp"

namespace axk::detail {

Result<std::vector<std::byte>> prepare_prog_payload(const ProgramSpec &program) {
    if (program.number == 0U || program.number > 128U || program.name.empty() || program.name.size() > 8U ||
        program.assignments.size() > maximum_program_assignments) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Program number or assignment count exceeds the encoded capacity")};
    }
    for (const auto &assignment : program.assignments) {
        if ((assignment.target_kind != "SBAC" && assignment.target_kind != "SBNK") || assignment.target_name.empty() ||
            assignment.target_name.size() > 16U) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "Program assignment cannot be represented by the object codec")};
        }
    }
    const auto capacity = std::max<std::size_t>(8U, program.assignments.size());
    const auto tail = prog_assignment_start + capacity * prog_assignment_stride;
    std::vector<std::byte> result(tail + prog_parameter_tail_size);
    ByteWriter writer{result};
    for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 3>{
             {{0x14U, 4U},
              {0x18U, static_cast<std::uint32_t>(result.size() - 0xe0U)},
              {0x1cU, static_cast<std::uint32_t>(result.size() - 0x30U)}}}) {
        if (auto written = writer.write_be32(offset, value); !written)
            return std::unexpected{written.error()};
    }
    const auto slot = std::format("{:03}", program.number);
    for (const auto &[offset, width, value] : std::array<std::tuple<std::size_t, std::size_t, std::string_view>, 3>{
             {{0U, 16U, "FSFSDEV3SPLXPROG"}, {0x32U, 16U, slot}, {0x78U, 8U, program.name}}}) {
        if (auto written = writer.write_ascii_field(offset, width, value); !written)
            return std::unexpected{written.error()};
    }
    result[0x30U] = std::byte{0x14};
    result[0x31U] = std::byte{0x0c};
    std::copy_n(result.begin() + 0x78, 3U, result.begin() + 0x6c);
    constexpr std::array<std::uint8_t, 24> common{0, 5, 0xff, 0xff, 0, 0,    0,    1,    0x40, 0,    0x40, 0x7f,
                                                  0, 0, 0,    0xfe, 0, 0x5a, 0x5a, 0x27, 0x78, 0xff, 0,    0};
    std::ranges::transform(common, result.begin() + 0x80, [](auto value) { return static_cast<std::byte>(value); });
    if (auto written = writer.write_be16(0x96U, static_cast<std::uint16_t>(program.assignments.size())); !written)
        return std::unexpected{written.error()};

    // Initialize unused capacity too; it is not part of the counted assignment list.
    for (std::size_t index = 0; index < capacity; ++index) {
        const auto offset = prog_assignment_start + index * prog_assignment_stride;
        for (const auto relative : {0x15U, 0x1dU, 0x23U, 0x24U, 0x28U, 0x2dU, 0x30U})
            result[offset + relative] = std::byte{0xff};
        result[offset + 0x1eU] = result[offset + 0x21U] = std::byte{0x7f};
        result[offset + 0x33U] = std::byte{1};
        if (index >= program.assignments.size())
            continue;
        const auto &assignment = program.assignments[index];
        if (auto written = writer.write_ascii_field(offset, 16U, assignment.target_name); !written)
            return std::unexpected{written.error()};
        result[offset + 0x14U] = assignment.target_kind == "SBAC" ? std::byte{0x11} : std::byte{0x10};
    }
    for (std::size_t index = 0; index < 6U; ++index) {
        const auto offset = index < 3U ? 0x98U + index * 0x28U : tail + (index - 3U) * 0x28U;
        result[offset] = std::byte{1};
        result[offset + 1U] = result[offset + 2U] = std::byte{127};
    }
    constexpr std::array<std::uint8_t, 16> controls{0x5b, 8, 1, 32, 0x5d, 0x1a, 1, 32, 0x5e, 0x2c, 1, 32, 0, 0, 0, 0};
    for (const auto offset : {std::size_t{0x110U}, tail + 0x78U})
        std::ranges::transform(controls, result.begin() + static_cast<std::ptrdiff_t>(offset),
                               [](auto value) { return static_cast<std::byte>(value); });
    constexpr std::array<std::uint8_t, 14> ad_defaults{0xff, 0xff, 0, 0, 0, 1, 0x40, 0, 0x40, 0, 1, 0x40, 0, 0x40};
    std::ranges::transform(ad_defaults, result.begin() + static_cast<std::ptrdiff_t>(tail + 0x88U),
                           [](auto value) { return static_cast<std::byte>(value); });
    std::fill_n(result.begin() + static_cast<std::ptrdiff_t>(tail + 0x96U), 16U, std::byte{64});
    result[tail + 0xa6U] = std::byte{4};
    if (auto applied =
            apply_program_parameters(result, program.parameters, program.model, ProgramParameterWriteMode::fresh);
        !applied)
        return std::unexpected{applied.error()};
    std::vector<ProgramAssignmentParameterPatch> patches;
    for (std::size_t index = 0; index < program.assignments.size(); ++index) {
        const auto &assignment = program.assignments[index];
        if (has_program_assignment_parameter_values(assignment.parameters))
            patches.push_back({index, assignment.target_kind, assignment.target_name, assignment.parameters});
    }
    if (auto applied = apply_program_assignment_patches(result, patches, program.model); !applied)
        return std::unexpected{applied.error()};
    return result;
}

} // namespace axk::detail
