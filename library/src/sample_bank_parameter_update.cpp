#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"

namespace axk::detail {
namespace {

constexpr std::size_t parameter_offset = 0xa8U;
constexpr std::size_t complete_parameter_bytes = 0xe0U;

Error invalid(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

std::int8_t signed_byte(const std::vector<std::byte> &payload, std::size_t offset) {
    return static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[offset]));
}

bool valid_loop_window(const CurrentSbnkMember &member, AudioSamplerLoopMode mode, std::uint32_t start,
                       std::uint32_t length) {
    const auto repeating =
        mode == AudioSamplerLoopMode::forward_loop || mode == AudioSamplerLoopMode::forward_loop_release;
    if (length == 0U)
        return !repeating && start == 0U;
    const auto wave_start = static_cast<std::uint64_t>(member.wave_start_frame);
    const auto wave_end = wave_start + member.wave_length_frames;
    const auto loop_start = static_cast<std::uint64_t>(start);
    return loop_start >= wave_start && loop_start <= wave_end && length <= wave_end - loop_start;
}

} // namespace

std::uint16_t sample_pitch_word(std::uint8_t root_key, std::int8_t fine_tune_cents, std::uint32_t sample_rate) {
    constexpr std::array<std::uint16_t, 12> fractions{0x000, 0x055, 0x0ab, 0x100, 0x155, 0x1ab,
                                                      0x200, 0x255, 0x2ab, 0x300, 0x355, 0x3ab};
    const auto root = root_key == 0U ? 0x03ab : ((root_key - 1U) / 12U) * 1024U + fractions[(root_key - 1U) % 12U];
    const auto rate = static_cast<int>(std::log(static_cast<double>(sample_rate) / 44'100.0) * 1477.3197);
    return static_cast<std::uint16_t>(static_cast<int>(root) - rate - fine_tune_cents);
}

Result<void> apply_sample_parameters_to_payload(std::vector<std::byte> &payload, const SampleParameters &overrides) {
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
    if (sample == nullptr || !sample->storage.structurally_valid)
        return std::unexpected{invalid("Sample storage format is not recognized for editing")};
    const auto native = sample->storage.format == SampleStorageFormat::a3000_188;
    const auto generation = native ? SampleParameterGeneration::a3000 : SampleParameterGeneration::a4000_a5000;

    SampleParameters effective;
    const auto changes_loop = overrides.loop_mode || overrides.loop_start_frame || overrides.loop_length_frames;
    const auto changes_expand = overrides.expand_detune || overrides.expand_dephase || overrides.expand_width;
    // A targeted edit validates its dependencies, not unrelated retained parameter state.
    if (overrides.root_key || overrides.key_low || overrides.key_high) {
        effective.root_key = sample->left.root_key;
        effective.key_low = sample->key_range_low;
        effective.key_high = sample->key_range_high;
    }
    if (changes_loop) {
        effective.loop_mode = static_cast<AudioSamplerLoopMode>(sample->loop_mode);
        effective.loop_start_frame = sample->left.loop_start_frame;
        effective.loop_length_frames = sample->left.loop_length_frames;
    }
    if (overrides.filter_scaling_break1 || overrides.filter_scaling_break2) {
        effective.filter_scaling_break1 = std::to_integer<std::uint8_t>(payload[0x10cU]);
        effective.filter_scaling_break2 = std::to_integer<std::uint8_t>(payload[0x10dU]);
    }
    if (changes_expand) {
        effective.expand_detune = signed_byte(payload, 0x112U);
        effective.expand_dephase = signed_byte(payload, 0x113U);
        effective.expand_width = signed_byte(payload, 0x114U);
    }
    if (overrides.velocity_high || overrides.velocity_low) {
        effective.velocity_high = sample->velocity_range_high;
        effective.velocity_low = sample->velocity_range_low;
    }
    if (overrides.level_scaling_break1 || overrides.level_scaling_break2) {
        effective.level_scaling_break1 = std::to_integer<std::uint8_t>(payload[0x11cU]);
        effective.level_scaling_break2 = std::to_integer<std::uint8_t>(payload[0x11dU]);
    }
    merge_sample_parameters(effective, overrides);
    if (auto valid = validate_sample_parameters(effective, generation); !valid)
        return std::unexpected{invalid("parameters are invalid for the existing Sample")};

    const auto mode = effective.loop_mode.value_or(static_cast<AudioSamplerLoopMode>(sample->loop_mode));
    const auto left_loop_start = overrides.loop_start_frame.value_or(sample->left.loop_start_frame);
    const auto left_loop_length = overrides.loop_length_frames.value_or(sample->left.loop_length_frames);
    const auto right_loop_start =
        overrides.loop_start_frame.value_or(sample->right ? sample->right->loop_start_frame : 0U);
    const auto right_loop_length =
        overrides.loop_length_frames.value_or(sample->right ? sample->right->loop_length_frames : 0U);
    const auto changes_pitch = overrides.root_key || overrides.fine_tune_cents;
    if ((changes_pitch && (sample->left.sample_rate == 0U || (sample->right && sample->right->sample_rate == 0U))) ||
        (changes_expand && sample->right_slot_present &&
         ((sample->sample_flags & 6U) != 0U || sample->right->wave_data_name == sample->left.wave_data_name) &&
         (effective.expand_detune.value_or(0) != 0 || effective.expand_dephase.value_or(0) != 0)) ||
        (changes_loop &&
         (!valid_loop_window(sample->left, mode, left_loop_start, left_loop_length) ||
          (sample->right && !valid_loop_window(*sample->right, mode, right_loop_start, right_loop_length))))) {
        return std::unexpected{invalid("parameters are invalid for the existing Sample")};
    }

    std::array<std::byte, complete_parameter_bytes> parameters{};
    const auto stored_parameter_bytes = sample->raw_parameter_window.size();
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(parameter_offset), stored_parameter_bytes,
                parameters.begin());
    if (auto applied = apply_sample_parameters_to_block(std::span{parameters}.first(stored_parameter_bytes), overrides,
                                                        native ? SampleParameterLayout::a3000
                                                               : SampleParameterLayout::a4000_a5000);
        !applied)
        return std::unexpected{invalid(applied.error().message)};
    ByteWriter writer{parameters};
    if (overrides.root_key) {
        parameters[0x2eU] = static_cast<std::byte>(*overrides.root_key);
        if (sample->right_slot_present)
            parameters[0x2fU] = static_cast<std::byte>(*overrides.root_key);
    }
    if (overrides.fine_tune_cents) {
        parameters[0x34U] = static_cast<std::byte>(static_cast<std::uint8_t>(*overrides.fine_tune_cents));
        if (sample->right_slot_present)
            parameters[0x35U] = static_cast<std::byte>(static_cast<std::uint8_t>(*overrides.fine_tune_cents));
    }
    if (changes_pitch) {
        const auto left_root = overrides.root_key.value_or(sample->left.root_key);
        const auto right_root = overrides.root_key.value_or(sample->right ? sample->right->root_key : left_root);
        const auto left_fine = overrides.fine_tune_cents.value_or(sample->left.fine_tune_cents);
        if (auto written = writer.write_be16(0x36U, sample_pitch_word(left_root, left_fine, sample->left.sample_rate));
            !written) {
            return std::unexpected{written.error()};
        }
        if (sample->right) {
            const auto right_fine = overrides.fine_tune_cents.value_or(sample->right->fine_tune_cents);
            if (auto written =
                    writer.write_be16(0x38U, sample_pitch_word(right_root, right_fine, sample->right->sample_rate));
                !written) {
                return std::unexpected{written.error()};
            }
        }
    }
    if (overrides.loop_start_frame && sample->right_slot_present) {
        if (auto written = writer.write_be32(0x54U, *overrides.loop_start_frame); !written)
            return std::unexpected{written.error()};
    }
    if (overrides.loop_length_frames && sample->right_slot_present) {
        if (auto written = writer.write_be32(0x5cU, *overrides.loop_length_frames); !written)
            return std::unexpected{written.error()};
    }
    if (overrides.loop_start_frame || overrides.loop_length_frames) {
        if (auto written = writer.write_be32(0xb8U, left_loop_start + left_loop_length); !written)
            return std::unexpected{written.error()};
    }
    if (!sample->right_slot_present && (overrides.expand_detune || overrides.expand_dephase)) {
        auto flags = std::to_integer<std::uint8_t>(parameters[0x28U]);
        flags = effective.expand_detune.value_or(0) != 0 || effective.expand_dephase.value_or(0) != 0
                    ? static_cast<std::uint8_t>(flags | 0x04U)
                    : static_cast<std::uint8_t>(flags & 0xfbU);
        parameters[0x28U] = static_cast<std::byte>(flags);
    }
    std::copy_n(parameters.begin(), stored_parameter_bytes,
                payload.begin() + static_cast<std::ptrdiff_t>(parameter_offset));
    return {};
}

} // namespace axk::detail
