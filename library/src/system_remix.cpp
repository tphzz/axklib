#include "axklib/system_file.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "system_remix_internal.hpp"

namespace axk {
namespace {

Error invalid(const char *message) { return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message); }

Result<void> validate_steps(std::span<const SystemRemixStep> steps) {
    if (steps.size() > 8U)
        return std::unexpected{invalid("Remix whole-eighth recipes cannot exceed eight steps")};
    unsigned eighths{};
    for (const auto &step : steps) {
        const auto duration = static_cast<unsigned>(step.duration);
        if (duration != 1U && duration != 2U)
            return std::unexpected{
                invalid("Remix duration requires an eighth or quarter; finer slices need source context")};
        const auto flags = static_cast<unsigned>(step.processing);
        switch (step.processing) {
        case SystemRemixProcessing::copy:
        case SystemRemixProcessing::random_slice:
        case SystemRemixProcessing::reverse_random_slice:
        case SystemRemixProcessing::fixed_slice:
        case SystemRemixProcessing::silence:
        case SystemRemixProcessing::lo_fi_random_slice:
        case SystemRemixProcessing::pitch_random_slice:
            break;
        default:
            return std::unexpected{invalid("Remix processing is unsupported; Gate is not independently authorable")};
        }
        if (((flags & 1U) == 0U || (flags & 4U) != 0U) && step.random_choice != 0U)
            return std::unexpected{invalid("Remix copy, fixed-source and silence steps require a zero random choice")};
        eighths += duration;
    }
    if (!steps.empty() && eighths != 8U)
        return std::unexpected{invalid("Remix step durations must total eight eighths")};
    return {};
}

} // namespace

Result<void> detail::validate_system_remix_selection(std::span<const std::byte> global, std::uint8_t slot) {
    if (global.size() != 0x1c0U || slot >= 5U)
        return std::unexpected{invalid("Remix selection requires a valid SYSTEM2 recipe slot")};
    const auto offset = static_cast<std::size_t>(slot) * 24U;
    std::vector<SystemRemixStep> steps;
    for (std::size_t i = 0; i < 24U; ++i) {
        const auto duration = std::to_integer<std::uint8_t>(global[0x50U + offset + i]);
        if (duration == 0U)
            return validate_steps(steps);
        steps.push_back({static_cast<SystemRemixDuration>(duration),
                         static_cast<SystemRemixProcessing>(std::to_integer<std::uint8_t>(global[0x140U + offset + i])),
                         std::to_integer<std::uint8_t>(global[0xc8U + offset + i])});
    }
    return std::unexpected{invalid("Remix recipe has no terminator within its stored lane")};
}

Result<DecodedSystemFile> patch_system_registered_remix(const DecodedSystemFile &file,
                                                        std::span<const SystemRegisteredRemixPatch> patches,
                                                        ASeriesModel model) {
    const bool native = model == ASeriesModel::a3000;
    if ((model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000) ||
        file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Remix target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (patches.empty())
        return file;
    if (native)
        return std::unexpected{invalid("A3000 SYSTEM has no registered Remix recipes")};
    if (patches.size() > 5U)
        return std::unexpected{invalid("A Remix batch cannot contain more than five slots")};
    std::array<bool, 5> seen{};
    auto global = std::span{*encoded}.subspan(current_record_envelope_size + 0x30U, 0x1c0U);
    for (const auto &patch : patches) {
        if (patch.slot >= seen.size() || seen[patch.slot])
            return std::unexpected{invalid("Remix slot is out of range or duplicated")};
        seen[patch.slot] = true;
        if (const auto valid = validate_steps(patch.steps); !valid)
            return std::unexpected{valid.error()};
        const auto offset = static_cast<std::size_t>(patch.slot) * 24U;
        for (const auto lane : {0x50U, 0xc8U, 0x140U})
            std::ranges::fill(global.subspan(lane + offset, 24U), std::byte{});
        for (std::size_t i = 0; i < patch.steps.size(); ++i) {
            global[0x50U + offset + i] = static_cast<std::byte>(patch.steps[i].duration);
            global[0xc8U + offset + i] = static_cast<std::byte>(patch.steps[i].random_choice);
            global[0x140U + offset + i] = static_cast<std::byte>(patch.steps[i].processing);
        }
    }
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
