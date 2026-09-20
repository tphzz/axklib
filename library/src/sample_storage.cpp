#include "axklib/sample_storage.hpp"

#include <algorithm>
#include <string_view>

#include "axklib/bytes.hpp"

namespace axk {

std::string_view sample_storage_format_name(SampleStorageFormat format) {
    switch (format) {
    case SampleStorageFormat::a3000_188:
        return "a3000_188";
    case SampleStorageFormat::a4000_a5000_224:
        return "a4000_a5000_224";
    default:
        return "unknown";
    }
}

SampleStorageInfo inspect_sample_storage(std::span<const std::byte> payload) {
    SampleStorageInfo result;
    constexpr std::string_view signature = "FSFSDEV3SPLXSBNK";
    if (payload.size() < 0x30U ||
        !std::equal(signature.begin(), signature.end(), payload.begin(), [](char a, std::byte b) {
            return static_cast<unsigned char>(a) == std::to_integer<unsigned>(b);
        })) {
        result.diagnostics.emplace_back("A complete Sample object header is required.");
        return result;
    }
    const ByteReader reader{payload};
    result.header_revision = *reader.be32(0x14U);
    result.older_body_bytes = *reader.be32(0x18U);
    result.later_body_bytes = *reader.be32(0x1cU);
    // A3000 V2 uses +18; MAIN 1.07/1.50 use +1c for revision 4.
    if (result.header_revision != 2U && result.header_revision != 4U) {
        result.diagnostics.emplace_back("This Sample header revision has no verified editable layout.");
        return result;
    }
    const auto body = result.header_revision == 2U ? result.older_body_bytes : result.later_body_bytes;
    if (body < 0x78U) {
        result.diagnostics.emplace_back("The declared Sample body does not contain its parameter prefix.");
        return result;
    }
    result.parameter_bytes = static_cast<std::size_t>(body - 0x78U);
    if (result.older_body_bytes != 0x134U || (result.header_revision == 2U ? body != 0x134U : body != 0x158U)) {
        result.diagnostics.emplace_back("The Sample header lengths do not describe a recognized stored format.");
        return result;
    }
    if (static_cast<std::uint64_t>(body) + 0x30U > payload.size()) {
        result.diagnostics.emplace_back("The declared Sample parameter block is truncated.");
        return result;
    }
    result.structurally_valid = true;
    result.format =
        result.header_revision == 2U ? SampleStorageFormat::a3000_188 : SampleStorageFormat::a4000_a5000_224;
    return result;
}

Result<SampleParameterBlocks> read_sample_parameter_blocks(std::span<const std::byte> payload) {
    const auto storage = inspect_sample_storage(payload);
    if (!storage.structurally_valid)
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::object, "Sample storage format is not recognized")};
    SampleParameterBlocks result;
    std::ranges::copy(payload.subspan(0xa8U, result.prefix.size()), result.prefix.begin());
    if (storage.format == SampleStorageFormat::a4000_a5000_224) {
        result.extension.emplace();
        std::ranges::copy(payload.subspan(0x164U, result.extension->size()), result.extension->begin());
    }
    return result;
}

std::optional<SampleParameterGeneration> sample_parameter_generation(SampleStorageFormat format) {
    if (format == SampleStorageFormat::a3000_188)
        return SampleParameterGeneration::a3000;
    if (format == SampleStorageFormat::a4000_a5000_224)
        return SampleParameterGeneration::a4000_a5000;
    return std::nullopt;
}

} // namespace axk
