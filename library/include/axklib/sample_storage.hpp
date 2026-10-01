#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/sample_parameters.hpp"

namespace axk {

enum class SampleStorageFormat : std::uint8_t { unknown, a3000_188, a4000_a5000_224 };

struct SampleStorageInfo {
    SampleStorageFormat format{SampleStorageFormat::unknown};
    bool structurally_valid{};
    std::uint32_t header_revision{};
    std::uint32_t older_body_bytes{};
    std::uint32_t later_body_bytes{};
    std::optional<std::size_t> parameter_bytes;
    std::vector<std::string> diagnostics;
};

struct SampleParameterBlocks {
    std::array<std::byte, 188> prefix{};
    std::optional<std::array<std::byte, 36>> extension;
};

AXK_API std::string_view sample_storage_format_name(SampleStorageFormat format);
AXK_API SampleStorageInfo inspect_sample_storage(std::span<const std::byte> payload);
AXK_API SampleStorageInfo inspect_sample_bank_storage(std::span<const std::byte> payload);
AXK_API Result<SampleParameterBlocks> read_sample_parameter_blocks(std::span<const std::byte> payload);
AXK_API Result<SampleParameterBlocks> read_sample_bank_parameter_blocks(std::span<const std::byte> payload);
AXK_API std::optional<SampleParameterGeneration> sample_parameter_generation(SampleStorageFormat format);

} // namespace axk
