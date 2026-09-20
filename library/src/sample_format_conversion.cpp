#include "axklib/sample_format_conversion.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/sample_format_conversion_internal.hpp"
#include "axklib/sample_parameter_codec.hpp"

namespace axk::detail {
Result<std::vector<std::byte>> extend_sample_parameter_prefix(std::span<const std::byte> prefix) {
    if (prefix.size() != 188U)
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Sample conversion requires the complete 188-byte prefix")};
    std::vector<std::byte> result(prefix.begin(), prefix.end());
    result.resize(224U);
    // MAIN 1.07 d4a50 / 1.50 de878 preserve all 188 bytes and initialize only the extension.
    std::copy_n(prefix.begin(), 0x18U, result.begin() + 0xbcU);
    const auto flags = std::to_integer<std::uint8_t>(prefix[0x29U]);
    result[0xd4U] = result[0xd5U] = (flags & 8U) != 0U ? std::byte{5} : std::byte{0};
    std::copy_n(prefix.begin() + 0xa5U, 4U, result.begin() + 0xd6U);
    result[0xdaU] = static_cast<std::byte>(flags & 1U);
    result[0xdbU] = result[0xdcU] = std::byte{90};
    return result;
}
} // namespace axk::detail

namespace axk {
namespace {
void blocked(SampleFormatConversionPlan &plan, std::string key, std::string message,
             std::optional<std::int64_t> value = std::nullopt) {
    plan.blockers.push_back({std::move(key), std::move(message), value});
}

void assess(SampleFormatConversionPlan &plan, std::span<const std::byte> block, SampleParameterGeneration generation) {
    const auto issues = assess_sample_parameter_block(block, generation);
    plan.blockers.insert(plan.blockers.end(), issues.begin(), issues.end());
    const auto decoded = decode_sample_parameter_block(block, generation);
    if (decoded) {
        if (const auto valid = detail::validate_sample_parameter_fields(decoded->parameters, generation); !valid)
            blocked(plan, "parameters", valid.error().message);
    }
}

bool compatibility_copy(std::size_t offset) { return offset < 0x18U || (offset >= 0xa5U && offset <= 0xa8U); }

std::vector<std::byte> downgrade(SampleFormatConversionPlan &plan, std::span<const std::byte> block) {
    std::vector<std::byte> prefix(block.begin(), block.begin() + 188);
    const auto high = std::to_integer<std::uint8_t>(block[0xd4U]);
    const auto low = std::to_integer<std::uint8_t>(block[0xd5U]);
    if (!((high == 0U && low == 0U) || (high == 5U && low == 5U)))
        blocked(plan, "velocity_crossfade", "A3000 supports only Off (0/0) or On (5/5), not these independent widths.");
    const auto portamento = std::to_integer<std::uint8_t>(block[0xdaU]);
    if (portamento > 1U)
        blocked(plan, "portamento_type", "A3000 supports only Off or Program portamento.", portamento);
    for (const auto &[key, offset] : std::array<std::pair<const char *, std::size_t>, 2>{
             {{"portamento_rate", 0xdbU}, {"portamento_time", 0xdcU}}}) {
        if (block[offset] != std::byte{90})
            blocked(plan, key, "This value would be lost. Set it to the conversion default 90 before converting.",
                    std::to_integer<std::uint8_t>(block[offset]));
    }
    if ((std::to_integer<std::uint8_t>(block[0x29U]) & 0xc0U) != 0U)
        blocked(plan, "sample_eq_type", "A3000 Sample EQ supports Peak/Dip only.");
    if (std::ranges::any_of(block.subspan(0xddU, 3U), [](std::byte value) { return value != std::byte{0}; }))
        blocked(plan, "extension", "Unknown nonzero extension bytes cannot be discarded.");
    std::ranges::copy(block.subspan(0xbcU, 24U), prefix.begin());
    std::ranges::copy(block.subspan(0xd6U, 4U), prefix.begin() + 0xa5U);
    const auto flags = std::to_integer<std::uint8_t>(prefix[0x29U]);
    prefix[0x29U] = static_cast<std::byte>((flags & 0xf6U) | (portamento == 1U ? 1U : 0U) | (high == 5U ? 8U : 0U));
    assess(plan, prefix, SampleParameterGeneration::a3000);
    const auto roundtrip = detail::extend_sample_parameter_prefix(prefix);
    if (roundtrip && plan.blockers.empty()) {
        for (std::size_t offset = 0; offset < block.size(); ++offset) {
            const auto mask = offset == 0x29U ? 0xf6U : 0xffU;
            if (!compatibility_copy(offset) && (std::to_integer<unsigned>((*roundtrip)[offset]) & mask) !=
                                                   (std::to_integer<unsigned>(block[offset]) & mask)) {
                blocked(plan, "roundtrip", "The stored parameters cannot be reproduced after a format round trip.");
                break;
            }
        }
    }
    plan.changes = {"Store a 188-byte A3000 parameter block and remove the 36-byte extension.",
                    "Copy authoritative controller and output settings into the A3000 prefix.",
                    "Represent velocity crossfade and portamento with A3000 switches.",
                    "Keep the Sample identity, name, relationships and Wave Data unchanged."};
    return prefix;
}
} // namespace

SampleFormatConversionPlan plan_sample_format_conversion(std::span<const std::byte> payload,
                                                         SampleStorageFormat target) {
    SampleFormatConversionPlan plan;
    plan.source = inspect_sample_storage(payload);
    plan.target = target;
    if (!plan.source.structurally_valid || target == SampleStorageFormat::unknown ||
        (target != SampleStorageFormat::a3000_188 && target != SampleStorageFormat::a4000_a5000_224)) {
        blocked(plan, "format", "Conversion requires recognized source and target Sample formats.");
        return plan;
    }
    plan.no_op = plan.source.format == target;
    if (plan.no_op) {
        plan.converted_payload.assign(payload.begin(), payload.end());
        return plan;
    }
    const auto native = plan.source.format == SampleStorageFormat::a3000_188;
    const auto block = payload.subspan(0xa8U, *plan.source.parameter_bytes);
    assess(plan, block, native ? SampleParameterGeneration::a3000 : SampleParameterGeneration::a4000_a5000);
    const auto header_reserved = payload.subspan(0x1cU, native ? 4U : 0U);
    if (std::ranges::any_of(header_reserved, [](std::byte value) { return value != std::byte{0}; }))
        blocked(plan, "header", "Uninterpreted header data would be changed by conversion.");
    std::vector<std::byte> converted;
    if (native) {
        if ((std::to_integer<std::uint8_t>(block[0x29U]) & 0xc0U) != 0U)
            blocked(plan, "sample_eq_type", "Retained A3000 flag bits would select a different EQ interpretation.");
        const auto extended = detail::extend_sample_parameter_prefix(block);
        if (extended) {
            converted = *extended;
            assess(plan, converted, SampleParameterGeneration::a4000_a5000);
        }
        plan.changes = {"Store a 224-byte A4000/A5000 parameter block with a 36-byte extension.",
                        "Preserve the 188-byte prefix and initialize controllers, outputs, crossfades and portamento.",
                        "The Sample becomes a4k/a5k format even if no later-only settings are used.",
                        "Keep the Sample identity, name, relationships and Wave Data unchanged."};
    } else {
        converted = downgrade(plan, block);
    }
    if (!plan.blockers.empty())
        return plan;
    plan.converted_payload.assign(payload.begin(), payload.begin() + 0xa8U);
    plan.converted_payload.insert(plan.converted_payload.end(), converted.begin(), converted.end());
    // Allocation padding belongs to the source container, not to the parameter format.
    const auto padding = payload.subspan(0xa8U + block.size());
    plan.converted_payload.insert(plan.converted_payload.end(), padding.begin(), padding.end());
    ByteWriter writer{plan.converted_payload};
    for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 3>{
             {{0x14U, native ? 4U : 2U}, {0x18U, 0x134U}, {0x1cU, native ? 0x158U : 0U}}}) {
        if (const auto written = writer.write_be32(offset, value); !written) {
            blocked(plan, "header", written.error().message);
            plan.converted_payload.clear();
            return plan;
        }
    }
    return plan;
}

} // namespace axk
