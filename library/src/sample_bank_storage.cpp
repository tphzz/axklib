#include "axklib/sample_storage.hpp"

#include <algorithm>
#include <string_view>

#include "axklib/bytes.hpp"

namespace axk {
SampleStorageInfo inspect_sample_bank_storage(std::span<const std::byte> payload) {
    SampleStorageInfo result;
    constexpr std::string_view signature = "FSFSDEV3SPLXSBAC";
    if (payload.size() < 0x14cU ||
        !std::equal(signature.begin(), signature.end(), payload.begin(), [](char a, std::byte b) {
            return static_cast<unsigned char>(a) == std::to_integer<unsigned>(b);
        })) {
        result.diagnostics.emplace_back("A complete Sample Bank header and parameter prefix are required.");
        return result;
    }
    const ByteReader reader{payload};
    if (*reader.u8(0x30U) != 0x11U) {
        result.diagnostics.emplace_back("The stored object class does not identify a Sample Bank.");
        return result;
    }
    result.header_revision = *reader.be32(0x14U);
    result.older_body_bytes = *reader.be32(0x18U);
    result.later_body_bytes = *reader.be32(0x1cU);
    if (result.header_revision != 2U && result.header_revision != 4U) {
        result.diagnostics.emplace_back("This Sample Bank header revision has no verified editable layout.");
        return result;
    }
    const bool native = result.header_revision == 2U;
    const auto body = native ? result.older_body_bytes : result.later_body_bytes;
    const auto logical_size = static_cast<std::uint64_t>(body) + 0x30U;
    const auto tail = native ? 0U : 36U;
    if ((!native && static_cast<std::uint64_t>(result.older_body_bytes) + tail != body) ||
        logical_size < 0x14cU + tail || logical_size > payload.size()) {
        result.diagnostics.emplace_back("The Sample Bank lengths do not describe a complete stored format.");
        return result;
    }
    const auto rows = logical_size - tail - 0x14cU;
    const auto count = *reader.u8(0x144U);
    if (rows % 20U != 0U || count > 127U || count > rows / 20U) {
        result.diagnostics.emplace_back("The Sample Bank member count or row capacity is invalid.");
        return result;
    }
    result.structurally_valid = true;
    result.parameter_bytes = native ? 188U : 224U;
    result.format = native ? SampleStorageFormat::a3000_188 : SampleStorageFormat::a4000_a5000_224;
    return result;
}

Result<SampleParameterBlocks> read_sample_bank_parameter_blocks(std::span<const std::byte> payload) {
    const auto storage = inspect_sample_bank_storage(payload);
    if (!storage.structurally_valid)
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Sample Bank storage format is not recognized")};
    SampleParameterBlocks result;
    std::ranges::copy(payload.subspan(0x78U, result.prefix.size()), result.prefix.begin());
    if (storage.format == SampleStorageFormat::a4000_a5000_224) {
        result.extension.emplace();
        const auto tail = static_cast<std::size_t>(storage.later_body_bytes) + 0x30U - 36U;
        std::ranges::copy(payload.subspan(tail, 36U), result.extension->begin());
    }
    return result;
}
} // namespace axk
