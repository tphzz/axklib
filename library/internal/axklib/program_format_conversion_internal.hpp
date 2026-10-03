#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "axklib/program_format_conversion.hpp"

namespace axk::detail {

std::array<std::byte, 176> program_extension_defaults();
void assess_program_conversion_parameters(ProgramFormatConversionPlan &plan, std::span<const std::byte> payload,
                                          const ProgramStorageInfo &storage);

} // namespace axk::detail
