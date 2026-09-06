#include "axklib/program_parameter_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/prog_codec.hpp"

namespace axk::detail {
namespace {

using Parameters = ProgramAssignmentParameters;

struct SignedField {
    std::optional<std::int8_t> Parameters::*member;
    std::size_t offset;
    int minimum;
    int maximum;
};

constexpr auto signed_fields = std::to_array<SignedField>({
    {&Parameters::level_offset, 0x16, -127, 127},
    {&Parameters::velocity_sensitivity_offset, 0x17, -127, 127},
    {&Parameters::pan_offset, 0x18, -127, 127},
    {&Parameters::high_velocity_crossfade_offset, 0x19, -127, 127},
    {&Parameters::fine_tune_offset, 0x1a, -127, 127},
    {&Parameters::low_velocity_crossfade_offset, 0x1b, -127, 127},
    {&Parameters::coarse_tune_offset, 0x1c, -127, 127},
    {&Parameters::key_shift, 0x20, -127, 127},
    {&Parameters::alternate_group, 0x24, -1, 16},
    {&Parameters::amp_attack_offset, 0x25, -127, 127},
    {&Parameters::amp_decay_offset, 0x26, -127, 127},
    {&Parameters::amp_release_offset, 0x27, -127, 127},
    {&Parameters::filter_cutoff_offset, 0x29, -127, 127},
    {&Parameters::filter_gain_offset, 0x2a, -63, 63},
    {&Parameters::filter_q_offset, 0x2b, -31, 31},
    {&Parameters::filter_cutoff_distance_offset, 0x2c, -127, 127},
    {&Parameters::output1_level_offset, 0x2f, -127, 127},
    {&Parameters::output2_level_offset, 0x32, -127, 127},
});

constexpr auto limits = std::to_array<std::pair<std::optional<std::uint8_t> Parameters::*, std::size_t>>({
    {&Parameters::key_high, 0x1e},
    {&Parameters::key_low, 0x1f},
    {&Parameters::velocity_high, 0x21},
    {&Parameters::velocity_low, 0x22},
});

constexpr auto switches = std::to_array<std::pair<std::optional<ProgramInheritableSwitch> Parameters::*, unsigned>>({
    {&Parameters::portamento, 0},
    {&Parameters::mono, 2},
    {&Parameters::key_crossfade, 4},
});

Error invalid(const char *message) { return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, message); }

template <typename T> std::optional<T> known(T value, int minimum, int maximum) {
    return static_cast<int>(value) < minimum || static_cast<int>(value) > maximum ? std::nullopt
                                                                                  : std::optional<T>{value};
}

void project_outputs(std::span<std::byte> row) {
    const auto output1 = std::to_integer<unsigned>(row[0x1d]);
    const auto output2 = std::to_integer<unsigned>(row[0x28]);
    row[0x2d] = row[0x30] = std::byte{0xff};
    if (output2 >= 1U && output2 <= 5U) {
        row[0x30] = row[0x28];
        row[0x31] = row[0x32];
    } else if (output2 >= 6U && output2 <= 9U) {
        row[0x2d] = static_cast<std::byte>(output2 - 5U);
        row[0x2e] = row[0x32];
    }
    if (output1 >= 1U && output1 <= 4U) {
        row[0x2d] = row[0x1d];
        row[0x2e] = row[0x2f];
    } else if (output1 >= 5U && output1 <= 9U) {
        row[0x30] = static_cast<std::byte>(output1 - 4U);
        row[0x31] = row[0x2f];
    }
}

Result<void> apply_row(std::span<std::byte> row, const Parameters &parameters, ASeriesModel model) {
    const auto original_outputs = std::array{row[0x1d], row[0x2f], row[0x28], row[0x32]};
    for (const auto &field : signed_fields) {
        const auto &value = parameters.*field.member;
        if (!value)
            continue;
        if (*value < field.minimum || *value > field.maximum)
            return std::unexpected{invalid("Easy Edit value is outside its stored offset or replacement domain")};
        row[field.offset] = static_cast<std::byte>(static_cast<std::uint8_t>(*value));
    }
    for (const auto &[member, offset] : limits) {
        if (const auto &value = parameters.*member; value) {
            if (*value > 127U)
                return std::unexpected{invalid("Easy Edit key and velocity limits must be 0..127")};
            row[offset] = static_cast<std::byte>(*value);
        }
    }
    for (const auto &[member, shift] : switches) {
        if (const auto &value = parameters.*member; value) {
            const auto raw = static_cast<int>(*value);
            if (raw < -1 || raw > 1)
                return std::unexpected{invalid("Easy Edit switch must be inherit, off, or on")};
            const auto packed = static_cast<unsigned>(raw == -1 ? 3 : raw);
            row[0x23] =
                static_cast<std::byte>((std::to_integer<unsigned>(row[0x23]) & ~(3U << shift)) | (packed << shift));
        }
    }
    for (const auto &[value, offset] :
         std::array{std::pair{parameters.output1, 0x1dU}, std::pair{parameters.output2, 0x28U}}) {
        if (!value)
            continue;
        if (*value < -1 || *value > (model == ASeriesModel::a5000 ? 12 : 9))
            return std::unexpected{invalid("Easy Edit output replacement is not supported for the model")};
        row[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(*value));
    }
    const ByteReader reader{row};
    if ((parameters.key_low || parameters.key_high) && (*reader.u8(0x1e) > 127U || *reader.u8(0x1f) > *reader.u8(0x1e)))
        return std::unexpected{invalid("Easy Edit key limits are invalid after merging the patch")};
    if ((parameters.velocity_low || parameters.velocity_high) &&
        (*reader.u8(0x21) > 127U || *reader.u8(0x22) > *reader.u8(0x21)))
        return std::unexpected{invalid("Easy Edit velocity limits are invalid after merging the patch")};
    if (parameters.receive) {
        const auto encoded = encode_program_receive(*parameters.receive, model);
        if (!encoded)
            return std::unexpected{encoded.error()};
        row[0x15] = static_cast<std::byte>(*encoded);
    }
    if (parameters.midi_control)
        row[0x33] = static_cast<std::byte>(*parameters.midi_control);
    if (original_outputs != std::array{row[0x1d], row[0x2f], row[0x28], row[0x32]})
        project_outputs(row);
    return {};
}

} // namespace

std::optional<ProgramReceiveSetting> decode_program_receive(std::uint8_t raw) {
    if (raw == 0xffU)
        return ProgramReceiveInherit{};
    if (raw < 16U)
        return ProgramReceiveChannel{MidiPort::a, static_cast<std::uint8_t>(raw + 1U)};
    if (raw == 16U)
        return ProgramReceiveBasic{};
    if (raw <= 32U)
        return ProgramReceiveChannel{MidiPort::b, static_cast<std::uint8_t>(raw - 16U)};
    return std::nullopt;
}

Result<std::uint8_t> encode_program_receive(const ProgramReceiveSetting &receive, ASeriesModel model) {
    if (model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{invalid("Program receive selection requires an A4000 or A5000 target model")};
    if (std::holds_alternative<ProgramReceiveInherit>(receive))
        return 0xffU;
    if (std::holds_alternative<ProgramReceiveBasic>(receive))
        return 16U;
    const auto *channel = std::get_if<ProgramReceiveChannel>(&receive);
    if (!channel || channel->channel == 0U || channel->channel > 16U ||
        (channel->port != MidiPort::a && channel->port != MidiPort::b) ||
        (channel->port == MidiPort::b && model != ASeriesModel::a5000))
        return std::unexpected{invalid("Program receive channel is not supported for the model")};
    return static_cast<std::uint8_t>(channel->port == MidiPort::a ? channel->channel - 1U : channel->channel + 16U);
}

ProgramAssignmentParameters decode_program_assignment_parameters(std::span<const std::byte> row,
                                                                 ProgStorageLayout layout) {
    const ByteReader reader{row};
    Parameters result;
    result.receive = decode_program_receive(*reader.u8(0x15));
    const auto current = layout == ProgStorageLayout::current_split_parameter_tail;
    for (const auto &field : signed_fields) {
        // Current output lanes do not describe the legacy routing representation.
        if (current || field.offset < 0x2dU)
            result.*field.member = known(*reader.s8(field.offset), field.minimum, field.maximum);
    }
    for (const auto &[member, offset] : limits)
        result.*member = known(*reader.u8(offset), 0, 127);
    for (const auto &[member, shift] : switches) {
        const auto raw = (*reader.u8(0x23) >> shift) & 3U;
        if (raw != 2U)
            result.*member = static_cast<ProgramInheritableSwitch>(raw == 3U ? -1 : static_cast<int>(raw));
    }
    if (current) {
        result.output1 = known(*reader.s8(0x1d), -1, 12);
        result.output2 = known(*reader.s8(0x28), -1, 12);
    }
    if (*reader.u8(0x33) <= 1U)
        result.midi_control = *reader.u8(0x33) != 0U;
    return result;
}

Result<void> apply_program_assignment_patches(std::vector<std::byte> &payload,
                                              std::span<const ProgramAssignmentParameterPatch> patches,
                                              ASeriesModel model) {
    if (model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Program parameter writes require an A4000 or A5000 target model")};
    const auto object = decode_object(payload);
    if (!object)
        return std::unexpected{object.error()};
    const auto *program = std::get_if<CurrentProg>(&object->payload);
    if (!program || !program->layout.parameter_tail_offset)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Easy Edit writes require the current Program layout")};
    std::set<std::size_t> ordinals;
    auto bytes = payload;
    for (const auto &patch : patches) {
        if (!ordinals.insert(patch.ordinal).second)
            return std::unexpected{invalid("Duplicate Program assignment ordinal")};
        if (patch.ordinal >= program->assignments.size())
            return std::unexpected{invalid("Program assignment ordinal is outside the stored count")};
        const auto &row = program->assignments[patch.ordinal];
        const auto kind = row.kind == 0x10U ? "SBNK" : row.kind == 0x11U ? "SBAC" : "";
        if (row.name.empty() || (row.kind != 0x10U && row.kind != 0x11U) || row.name != patch.expected_target_name ||
            kind != patch.expected_target_kind)
            return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                              "Program assignment no longer matches the expected stored target")};
        if (auto applied =
                apply_row(std::span{bytes}.subspan(row.offset, prog_assignment_stride), patch.parameters, model);
            !applied)
            return applied;
    }
    payload = std::move(bytes);
    return {};
}

bool has_program_assignment_parameter_values(const ProgramAssignmentParameters &parameters) {
    return parameters.receive || parameters.output1 || parameters.output2 || parameters.midi_control ||
           std::ranges::any_of(signed_fields,
                               [&](const auto &field) { return (parameters.*field.member).has_value(); }) ||
           std::ranges::any_of(limits, [&](const auto &field) { return (parameters.*field.first).has_value(); }) ||
           std::ranges::any_of(switches, [&](const auto &field) { return (parameters.*field.first).has_value(); });
}

} // namespace axk::detail
