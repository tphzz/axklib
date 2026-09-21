#include "axklib/sample_format_conversion.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/sample_format_conversion_internal.hpp"

namespace axk {
SampleFormatConversionPlan plan_sample_bank_format_conversion(std::span<const std::byte> payload,
                                                              SampleStorageFormat target) {
    SampleFormatConversionPlan plan;
    plan.source = inspect_sample_bank_storage(payload);
    plan.target = target;
    if (!plan.source.structurally_valid ||
        (target != SampleStorageFormat::a3000_188 && target != SampleStorageFormat::a4000_a5000_224)) {
        plan.blockers.push_back(
            {"format", "Conversion requires recognized source and target Sample Bank formats.", {}});
        return plan;
    }
    plan.no_op = plan.source.format == target;
    if (plan.no_op) {
        plan.converted_payload.assign(payload.begin(), payload.end());
        return plan;
    }
    const bool native = plan.source.format == SampleStorageFormat::a3000_188;
    const ByteReader reader{payload};
    if (native && plan.source.later_body_bytes != 0U)
        plan.blockers.push_back({"header", "Uninterpreted header data would be changed by conversion.", {}});
    for (std::size_t word = 0; word < 3; ++word) {
        if (*reader.be32(0x134U + word * 4U) != 0U)
            plan.blockers.push_back({"active_overrides",
                                     "This bank has active overrides whose format "
                                     "conversion is not yet supported. They cannot be discarded.",
                                     {}});
    }
    const auto blocks = read_sample_bank_parameter_blocks(payload);
    if (!blocks) {
        plan.blockers.push_back({"parameters", blocks.error().message, {}});
        return plan;
    }
    std::vector<std::byte> block(blocks->prefix.begin(), blocks->prefix.end());
    if (blocks->extension)
        block.insert(block.end(), blocks->extension->begin(), blocks->extension->end());
    const auto converted = detail::convert_sample_parameter_block(plan, block);
    plan.changes = {native ? "The Sample Bank becomes a4k/a5k format." : "The Sample Bank becomes a3k format.",
                    "Keep the bank identity, name, Program links and member rows unchanged.",
                    "Member Samples and Wave Data remain unchanged, including their stored formats."};
    if (!plan.blockers.empty())
        return plan;
    const auto logical_end =
        static_cast<std::size_t>(native ? plan.source.older_body_bytes : plan.source.later_body_bytes) + 0x30U;
    const auto rows_end = logical_end - (native ? 0U : 36U);
    const auto new_end = rows_end + (native ? 36U : 0U);
    if (new_end - 0x30U > std::numeric_limits<std::uint32_t>::max()) {
        plan.blockers.push_back({"size", "The converted Sample Bank exceeds the stored length range.", {}});
        return plan;
    }
    auto &result = plan.converted_payload;
    result.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(rows_end));
    std::copy_n(converted.begin(), 188, result.begin() + 0x78);
    if (native)
        result.insert(result.end(), converted.begin() + 188, converted.end());
    else
        std::copy_n(converted.begin(), 3, result.begin() + 0x6c);
    result.insert(result.end(), payload.begin() + static_cast<std::ptrdiff_t>(logical_end), payload.end());
    ByteWriter writer{result};
    for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 3>{
             {{0x14U, native ? 4U : 2U},
              {0x18U, static_cast<std::uint32_t>(rows_end - 0x30U)},
              {0x1cU, native ? static_cast<std::uint32_t>(new_end - 0x30U) : 0U}}}) {
        if (const auto written = writer.write_be32(offset, value); !written) {
            plan.blockers.push_back({"header", written.error().message, {}});
            result.clear();
            return plan;
        }
    }
    return plan;
}
} // namespace axk
