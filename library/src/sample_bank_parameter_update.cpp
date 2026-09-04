#include "axklib/writer_internal.hpp"

#include <array>
#include <cmath>
#include <cstdint>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"

namespace axk::detail {
namespace {

Error invalid(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

} // namespace

std::uint16_t sample_pitch_word(std::uint8_t root_key, std::int8_t fine_tune_cents, std::uint32_t sample_rate) {
    constexpr std::array<std::uint16_t, 12> fractions{0x000, 0x055, 0x0ab, 0x100, 0x155, 0x1ab,
                                                      0x200, 0x255, 0x2ab, 0x300, 0x355, 0x3ab};
    const auto root = root_key == 0U ? 0x03ab : ((root_key - 1U) / 12U) * 1024U + fractions[(root_key - 1U) % 12U];
    const auto rate = static_cast<int>(std::log(static_cast<double>(sample_rate) / 44'100.0) * 1477.3197);
    return static_cast<std::uint16_t>(static_cast<int>(root) - rate - fine_tune_cents);
}

Result<void> apply_sample_bank_parameter_overrides_to_payload(std::vector<std::byte> &payload,
                                                              const SampleBankParameterOverrides &overrides) {
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
    if (sample == nullptr || payload.size() <= 0x11bU)
        return std::unexpected{invalid("Sample Bank member is not an editable current Sample")};

    const auto left_root = overrides.root_key.value_or(sample->left.root_key);
    const auto right_root = overrides.root_key.value_or(sample->right ? sample->right->root_key : left_root);
    const auto key_low = overrides.key_low.value_or(sample->key_range_low);
    const auto key_high = overrides.key_high.value_or(sample->key_range_high);
    const auto velocity_low = overrides.velocity_low.value_or(sample->velocity_range_low);
    const auto velocity_high = overrides.velocity_high.value_or(sample->velocity_range_high);
    const auto detune =
        overrides.expand_detune.value_or(static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[0x112U])));
    const auto dephase =
        overrides.expand_dephase.value_or(static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[0x113U])));
    const auto width =
        overrides.expand_width.value_or(static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[0x114U])));
    const auto effective_low = key_low == sampler_original_key_low_limit ? left_root : key_low;
    const auto effective_high = key_high == sampler_original_key_high_limit ? left_root : key_high;
    const auto changes_pitch = overrides.root_key || overrides.fine_tune_cents;
    if (left_root > 127U || (changes_pitch && right_root > 127U) ||
        (key_low > 127U && key_low != sampler_original_key_low_limit) || key_high > sampler_original_key_high_limit ||
        effective_high < effective_low || velocity_low > 127U || velocity_high > 127U || velocity_high < velocity_low ||
        (changes_pitch && (sample->left.sample_rate == 0U || (sample->right && sample->right->sample_rate == 0U))) ||
        (overrides.expand_detune && (detune < -7 || detune > 7)) ||
        (overrides.expand_dephase && (dephase < -63 || dephase > 63)) ||
        (overrides.expand_width && (width < -63 || width > 63)) ||
        (sample->right_slot_present && (overrides.expand_detune || overrides.expand_dephase) &&
         (detune != 0 || dephase != 0))) {
        return std::unexpected{invalid("Sample Bank parameters are invalid for an existing member Sample")};
    }

    ByteWriter writer{payload};
    if (overrides.root_key) {
        payload[0xd6U] = static_cast<std::byte>(left_root);
        if (sample->right_slot_present)
            payload[0xd7U] = static_cast<std::byte>(right_root);
    }
    if (overrides.fine_tune_cents) {
        payload[0xdcU] = static_cast<std::byte>(static_cast<std::uint8_t>(*overrides.fine_tune_cents));
        if (sample->right_slot_present)
            payload[0xddU] = static_cast<std::byte>(static_cast<std::uint8_t>(*overrides.fine_tune_cents));
    }
    if (changes_pitch) {
        const auto left_fine = overrides.fine_tune_cents.value_or(sample->left.fine_tune_cents);
        if (auto written = writer.write_be16(0xdeU, sample_pitch_word(left_root, left_fine, sample->left.sample_rate));
            !written) {
            return std::unexpected{written.error()};
        }
        if (sample->right) {
            const auto right_fine = overrides.fine_tune_cents.value_or(sample->right->fine_tune_cents);
            if (auto written =
                    writer.write_be16(0xe0U, sample_pitch_word(right_root, right_fine, sample->right->sample_rate));
                !written) {
                return std::unexpected{written.error()};
            }
        }
    }
    if (overrides.key_high)
        payload[0xe2U] = static_cast<std::byte>(key_high);
    if (overrides.key_low)
        payload[0xe3U] = static_cast<std::byte>(key_low);
    if (overrides.expand_detune)
        payload[0x112U] = static_cast<std::byte>(static_cast<std::uint8_t>(detune));
    if (overrides.expand_dephase)
        payload[0x113U] = static_cast<std::byte>(static_cast<std::uint8_t>(dephase));
    if (overrides.expand_width)
        payload[0x114U] = static_cast<std::byte>(static_cast<std::uint8_t>(width));
    if (overrides.level)
        payload[0x116U] = static_cast<std::byte>(*overrides.level);
    if (overrides.velocity_high)
        payload[0x11aU] = static_cast<std::byte>(velocity_high);
    if (overrides.velocity_low)
        payload[0x11bU] = static_cast<std::byte>(velocity_low);
    if (!sample->right_slot_present && (overrides.expand_detune || overrides.expand_dephase)) {
        auto flags = std::to_integer<std::uint8_t>(payload[0xd0U]);
        flags = detune != 0 || dephase != 0 ? static_cast<std::uint8_t>(flags | 0x04U)
                                            : static_cast<std::uint8_t>(flags & 0xfbU);
        payload[0xd0U] = static_cast<std::byte>(flags);
    }
    return {};
}

} // namespace axk::detail
