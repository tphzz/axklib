#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/export.hpp"

namespace axk {

enum class ProgramStorageFormat : std::uint8_t { unknown, a3000, a4000_a5000 };

struct ProgramStorageInfo {
    ProgramStorageFormat format{ProgramStorageFormat::unknown};
    bool structurally_valid{};
    std::uint32_t header_revision{};
    std::size_t logical_size{};
    std::uint16_t stored_assignment_count{};
    std::size_t assignment_capacity{};
    std::size_t parameter_tail_bytes{};
    std::vector<std::string> diagnostics;
};

AXK_API std::string_view program_storage_format_name(ProgramStorageFormat format);
AXK_API ProgramStorageInfo inspect_program_storage(std::span<const std::byte> payload);

} // namespace axk
