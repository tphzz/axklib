#include "axklib/program_storage.hpp"

#include "axklib/object.hpp"
#include "axklib/prog_codec.hpp"

namespace axk {

std::string_view program_storage_format_name(ProgramStorageFormat format) {
    switch (format) {
    case ProgramStorageFormat::a3000:
        return "a3000";
    case ProgramStorageFormat::a4000_a5000:
        return "a4000_a5000";
    default:
        return "unknown";
    }
}

ProgramStorageInfo inspect_program_storage(std::span<const std::byte> payload) {
    ProgramStorageInfo result;
    const auto header = decode_object_header(payload);
    if (!header || header->type != ObjectType::prog || payload[0x30U] != std::byte{0x14}) {
        result.diagnostics.emplace_back("A complete Program object header is required.");
        return result;
    }
    result.header_revision = header->unknown_0x14;
    const auto layout = detail::decode_prog_layout(payload, *header);
    if (!layout) {
        result.diagnostics.push_back(layout.error().message);
        return result;
    }
    result.structurally_valid = true;
    result.format = layout->parameter_tail_offset ? ProgramStorageFormat::a4000_a5000 : ProgramStorageFormat::a3000;
    result.logical_size = layout->logical_size;
    result.stored_assignment_count = layout->stored_assignment_count;
    result.assignment_capacity = layout->assignment_capacity;
    result.parameter_tail_bytes = layout->parameter_tail_offset ? detail::prog_parameter_tail_size : 0U;
    return result;
}

} // namespace axk
