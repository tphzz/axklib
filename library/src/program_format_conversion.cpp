#include "axklib/program_format_conversion.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/program_format_conversion_internal.hpp"

namespace axk::detail {

std::array<std::byte, 176> program_extension_defaults() {
    // Shared current Program initialization for conversion and fresh authoring.
    std::array<std::byte, 176> result{};
    for (std::size_t slot = 0; slot < 3U; ++slot) {
        result[slot * 40U] = std::byte{1};
        result[slot * 40U + 1U] = result[slot * 40U + 2U] = std::byte{127};
    }
    constexpr std::array<std::uint8_t, 16> controls{0x5b, 8, 1, 32, 0x5d, 0x1a, 1, 32, 0x5e, 0x2c, 1, 32, 0, 0, 0, 0};
    std::ranges::transform(controls, result.begin() + 0x78, [](auto value) { return static_cast<std::byte>(value); });
    constexpr std::array<std::uint8_t, 14> ad{0xff, 0xff, 0, 0, 0, 1, 0x40, 0, 0x40, 0, 1, 0x40, 0, 0x40};
    std::ranges::transform(ad, result.begin() + 0x88, [](auto value) { return static_cast<std::byte>(value); });
    std::fill_n(result.begin() + 0x96, 16U, std::byte{64});
    result[0xa6] = std::byte{4};
    return result;
}

} // namespace axk::detail

namespace axk {
namespace {
constexpr std::array<std::size_t, 6> promoted_row_bytes{0x19U, 0x1bU, 0x1dU, 0x28U, 0x2fU, 0x32U};

void blocked(ProgramFormatConversionPlan &plan, std::string key, std::string message,
             std::optional<std::int64_t> value = std::nullopt) {
    plan.blockers.push_back({std::move(key), std::move(message), value});
}

void write_header(std::vector<std::byte> &bytes, std::size_t logical, bool native) {
    ByteWriter writer{bytes};
    (void)writer.write_be32(0x14U, native ? 2U : 4U);
    (void)writer.write_be32(0x18U, static_cast<std::uint32_t>(logical - (native ? 0x30U : 0xe0U)));
    (void)writer.write_be32(0x1cU, native ? 0U : static_cast<std::uint32_t>(logical - 0x30U));
}

std::vector<std::byte> upgrade(std::span<const std::byte> payload, const ProgramStorageInfo &storage) {
    const auto logical = storage.logical_size;
    const auto tail = detail::program_extension_defaults();
    std::vector<std::byte> result(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(logical));
    result.insert(result.end(), tail.begin(), tail.end());
    result.insert(result.end(), payload.begin() + static_cast<std::ptrdiff_t>(logical), payload.end());
    for (std::size_t slot = 0; slot < 3U; ++slot) {
        const auto offset = 0x98U + slot * 0x28U;
        auto type = std::to_integer<unsigned>(result[offset + 7U]);
        if (storage.header_revision == 1U && type >= 52U)
            type -= 5U;
        result[offset + 6U] = result[offset + 7U] = static_cast<std::byte>(type);
    }
    std::copy_n(payload.begin() + 0x110, 16U, result.begin() + static_cast<std::ptrdiff_t>(logical + 0x78U));
    std::copy_n(payload.begin() + 0x87, 4U, result.begin() + static_cast<std::ptrdiff_t>(logical + 0x8dU));
    std::copy_n(payload.begin() + 0x86, 5U, result.begin() + static_cast<std::ptrdiff_t>(logical + 0x91U));
    for (std::size_t row = 0; row < storage.stored_assignment_count; ++row) {
        const auto start = 0x120U + row * 0x38U;
        const auto velocity = std::to_integer<unsigned>(payload[start + 0x23U]) >> 6U;
        result[start + 0x19U] = result[start + 0x1bU] = velocity == 0U   ? std::byte{0xfb}
                                                        : velocity == 1U ? std::byte{5}
                                                                         : std::byte{};
        for (const auto &[from, to] : std::array{std::pair{0x2dU, 0x1dU}, std::pair{0x2eU, 0x2fU},
                                                 std::pair{0x30U, 0x28U}, std::pair{0x31U, 0x32U}})
            result[start + to] = payload[start + from];
    }
    write_header(result, logical + tail.size(), false);
    return result;
}

void native_promotion_guards(ProgramFormatConversionPlan &plan, std::span<const std::byte> payload,
                             const ProgramStorageInfo &storage) {
    if (*ByteReader{payload}.be32(0x1cU) != 0U)
        blocked(plan, "header", "Uninterpreted Program header data would be overwritten.");
    for (std::size_t slot = 0; slot < 3U; ++slot)
        if (payload[0x98U + slot * 0x28U + 6U] != std::byte{})
            blocked(plan, std::format("effects.{}.reserved", slot + 1U),
                    std::format("Effect {} contains retained native data in the later effect-type field.", slot + 1U));
    for (std::size_t row = 0; row < storage.stored_assignment_count; ++row) {
        const auto start = 0x120U + row * 0x38U;
        for (const auto relative : promoted_row_bytes)
            if (payload[start + relative] != std::byte{})
                blocked(
                    plan, std::format("assignments.{}.reserved", row + 1U),
                    std::format("Assignment {} contains retained native data in a later parameter field.", row + 1U));
    }
}

void generation_guards(ProgramFormatConversionPlan &plan, std::span<const std::byte> payload,
                       const ProgramStorageInfo &storage) {
    for (std::size_t row = 0; row < storage.stored_assignment_count; ++row) {
        const auto start = 0x120U + row * 0x38U;
        if (payload[start + 0x17U] != std::byte{})
            blocked(plan, std::format("assignments.{}.velocity_sensitivity", row + 1U),
                    std::format("Assignment {} velocity sensitivity is not shared by both formats.", row + 1U));
        if ((payload[start + 0x34U] & std::byte{0x20}) != std::byte{})
            blocked(
                plan, std::format("assignments.{}.extended_flag", row + 1U),
                std::format("Assignment {} contains an additional control flag that A3000 cannot preserve.", row + 1U));
    }
}

bool compatibility_byte(std::size_t offset, const ProgramStorageInfo &storage) {
    if (offset >= 0x14U && offset < 0x20U)
        return true;
    if ((offset >= 0x87U && offset <= 0x8aU) || (offset >= 0x110U && offset < 0x120U))
        return true;
    for (std::size_t slot = 0; slot < 3U; ++slot)
        if (offset == 0x98U + slot * 0x28U + 7U)
            return true;
    if (offset >= 0x120U && offset < 0x120U + storage.stored_assignment_count * 0x38U) {
        const auto relative = (offset - 0x120U) % 0x38U;
        return relative == 0x2dU || relative == 0x2eU || relative == 0x30U || relative == 0x31U;
    }
    return false;
}

std::string difference_label(std::size_t offset, const ProgramStorageInfo &storage) {
    const auto tail = storage.logical_size - storage.parameter_tail_bytes;
    if (offset < tail)
        return "Program settings";
    const auto relative = offset - tail;
    if (relative < 0x78U)
        return std::format("Effect {} settings", relative / 0x28U + 4U);
    if (relative < 0x88U)
        return "Program controllers";
    if (relative < 0x8cU)
        return "MIDI port B settings";
    if (relative == 0x8cU)
        return "Effects 4-6 connection";
    if (relative < 0x96U)
        return "Independent A/D routing or pan";
    if (relative < 0xa7U)
        return "StepWave settings";
    return "Retained extension data";
}

std::vector<std::byte> downgrade(ProgramFormatConversionPlan &plan, std::span<const std::byte> payload) {
    const auto &storage = plan.source;
    const auto tail = storage.logical_size - storage.parameter_tail_bytes;
    std::vector<std::byte> result(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(tail));
    result.insert(result.end(), payload.begin() + static_cast<std::ptrdiff_t>(storage.logical_size), payload.end());
    for (std::size_t slot = 0; slot < 3U; ++slot) {
        const auto offset = 0x98U + slot * 0x28U;
        result[offset + 7U] = result[offset + 6U];
        result[offset + 6U] = std::byte{};
    }
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(tail + 0x78U), 16U, result.begin() + 0x110);
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(tail + 0x8dU), 4U, result.begin() + 0x87);
    for (std::size_t row = 0; row < storage.stored_assignment_count; ++row) {
        const auto start = 0x120U + row * 0x38U;
        const auto high = payload[start + 0x19U];
        const auto low = payload[start + 0x1bU];
        if (high != low || (high != std::byte{0xfb} && high != std::byte{5} && high != std::byte{}))
            blocked(plan, std::format("assignments.{}.velocity_crossfade", row + 1U),
                    std::format("Assignment {} has independent velocity crossfades; A3000 requires -5/-5, 5/5 or 0/0.",
                                row + 1U));
        const auto velocity = std::to_integer<unsigned>(payload[start + 0x23U]) >> 6U;
        const auto promoted = velocity == 0U ? std::byte{0xfb} : velocity == 1U ? std::byte{5} : std::byte{};
        if (high != promoted || low != promoted)
            blocked(plan, std::format("assignments.{}.retained_crossfade", row + 1U),
                    std::format("Assignment {} velocity crossfades conflict with retained native data.", row + 1U));
        // This is the inverse of legacy load, not the lossy save-side compatibility projection.
        for (const auto &[from, to] : std::array{std::pair{0x1dU, 0x2dU}, std::pair{0x2fU, 0x2eU},
                                                 std::pair{0x28U, 0x30U}, std::pair{0x32U, 0x31U}})
            result[start + to] = payload[start + from];
        for (const auto relative : promoted_row_bytes)
            result[start + relative] = std::byte{};
    }
    write_header(result, tail, true);
    const auto native = inspect_program_storage(result);
    detail::assess_program_conversion_parameters(plan, result, native);
    if (!plan.blockers.empty())
        return {};
    const auto roundtrip = upgrade(result, native);
    std::vector<std::string> reported;
    for (std::size_t offset = 0; offset < payload.size(); ++offset) {
        if (compatibility_byte(offset, storage) || payload[offset] == roundtrip[offset])
            continue;
        const auto label = difference_label(offset, storage);
        if (std::ranges::find(reported, label) == reported.end()) {
            blocked(plan, std::format("roundtrip.{:x}", offset), label + " cannot be preserved in A3000 format.");
            reported.push_back(label);
        }
    }
    return result;
}
} // namespace

ProgramFormatConversionPlan plan_program_format_conversion(std::span<const std::byte> payload,
                                                           ProgramStorageFormat target) {
    ProgramFormatConversionPlan plan;
    plan.source = inspect_program_storage(payload);
    plan.target = target;
    if (!plan.source.structurally_valid ||
        (target != ProgramStorageFormat::a3000 && target != ProgramStorageFormat::a4000_a5000)) {
        blocked(plan, "format", "Conversion requires recognized source and target Program formats.");
        return plan;
    }
    plan.no_op = plan.source.format == target;
    if (plan.no_op) {
        plan.converted_payload.assign(payload.begin(), payload.end());
        return plan;
    }
    if (plan.source.logical_size > std::numeric_limits<std::uint32_t>::max() - 176U) {
        blocked(plan, "size", "The converted Program length exceeds the stored header range.");
        return plan;
    }
    detail::assess_program_conversion_parameters(plan, payload, plan.source);
    generation_guards(plan, payload, plan.source);
    if (plan.source.format == ProgramStorageFormat::a3000)
        native_promotion_guards(plan, payload, plan.source);
    if (!plan.blockers.empty())
        return plan;
    auto converted =
        plan.source.format == ProgramStorageFormat::a3000 ? upgrade(payload, plan.source) : downgrade(plan, payload);
    if (!plan.blockers.empty())
        return plan;
    const auto storage = inspect_program_storage(converted);
    if (!storage.structurally_valid || storage.format != target) {
        blocked(plan, "result", "The converted Program has an invalid stored layout.");
        return plan;
    }
    detail::assess_program_conversion_parameters(plan, converted, storage);
    if (!plan.blockers.empty())
        return plan;
    plan.changes = {target == ProgramStorageFormat::a3000
                        ? "Store the Program in A3000 V2 format."
                        : "Initialize the additional A4000/A5000 settings from the A3000 Program.",
                    "Translate effect, controller, A/D and assignment parameters without dropping settings.",
                    "Keep the Program slot, name, assignments and linked objects unchanged."};
    plan.converted_payload = std::move(converted);
    return plan;
}

} // namespace axk
