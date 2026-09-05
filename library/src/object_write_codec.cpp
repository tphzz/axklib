#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>

#include "axklib/bytes.hpp"

namespace axk {
namespace {

class ObjectPayloadWriter {
  public:
    explicit ObjectPayloadWriter(std::span<std::byte> payload) : writer_{payload} {}

    void be16(std::size_t offset, std::uint16_t value) {
        if (!failure_) {
            auto written = writer_.write_be16(offset, value);
            if (!written)
                failure_ = written.error();
        }
    }

    void be32(std::size_t offset, std::uint32_t value) {
        if (!failure_) {
            auto written = writer_.write_be32(offset, value);
            if (!written)
                failure_ = written.error();
        }
    }

    [[nodiscard]] Result<void> finish() const {
        if (failure_)
            return std::unexpected{*failure_};
        return {};
    }

  private:
    ByteWriter writer_;
    std::optional<Error> failure_;
};

Result<std::vector<std::byte>> ascii(std::string_view value, std::size_t size, std::byte pad = std::byte{' '}) {
    if (value.size() > size || !std::ranges::all_of(value, [](unsigned char character) { return character < 0x80U; })) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "writer name does not fit its ASCII field")};
    }
    std::vector<std::byte> result(size, pad);
    std::ranges::transform(value, result.begin(), [](char character) { return static_cast<std::byte>(character); });
    return result;
}

struct LoopWindow {
    std::uint32_t start{};
    std::uint32_t length{};
};

constexpr bool valid_loop_mode(AudioSamplerLoopMode mode) {
    return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(AudioSamplerLoopMode::reverse_one_shot);
}

constexpr bool requires_explicit_loop_window(AudioSamplerLoopMode mode) {
    return mode == AudioSamplerLoopMode::forward_loop || mode == AudioSamplerLoopMode::forward_loop_release;
}

Result<LoopWindow> loop_window(AudioSamplerLoopMode mode, std::uint32_t start, std::uint32_t length,
                               std::uint64_t frame_count) {
    if (frame_count > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(ErrorCode::audio_wave_data_too_large, ErrorCategory::audio,
                                          "sampler loop window exceeds the encoded frame range")};
    }
    if (!valid_loop_mode(mode)) {
        return std::unexpected{
            make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, "sampler loop mode is invalid")};
    }
    if (start == 0U && length == 0U && !requires_explicit_loop_window(mode))
        return LoopWindow{0U, static_cast<std::uint32_t>(frame_count)};
    if (length == 0U || start >= frame_count || length > frame_count - start) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "sampler loop window must be non-empty and remain inside Wave Data")};
    }
    return LoopWindow{start, length};
}

Result<std::vector<std::byte>> serialize_smpl(const WaveformSpec &spec, const ImportedAudio &audio,
                                              std::uint32_t reference_value, std::string_view embedded_container_name) {
    const auto pcm_bytes = audio.pcm_channels.size() == 1U ? audio.pcm_channels[0].size() : 0U;
    if (audio.output_frames > maximum_wave_data_frames_per_channel ||
        pcm_bytes > maximum_wave_data_pcm16_bytes_per_channel) {
        return std::unexpected{make_error(ErrorCode::audio_wave_data_too_large, ErrorCategory::audio,
                                          "Wave Data exceeds the 32 MiB per-channel A-series limit")};
    }
    if (audio.output_sample_width_bits != sampler_output_sample_width_bits || audio.pcm_channels.size() != 1U ||
        pcm_bytes % 2U != 0U || pcm_bytes / 2U != audio.output_frames ||
        audio.output_frames > std::numeric_limits<std::uint32_t>::max() || audio.output_sample_rate == 0U ||
        audio.output_sample_rate > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected{make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "SMPL writer requires bounded mono 16-bit PCM")};
    }
    auto loop = loop_window(spec.loop_mode, spec.loop_start_frame, spec.loop_length_frames, audio.output_frames);
    if (!loop)
        return std::unexpected{loop.error()};
    std::vector<std::byte> stored;
    stored.reserve(audio.pcm_channels[0].size() + 8U);
    for (std::size_t offset = 0; offset < audio.pcm_channels[0].size(); offset += 2U) {
        stored.push_back(audio.pcm_channels[0][offset + 1U]);
        stored.push_back(audio.pcm_channels[0][offset]);
    }
    stored.insert(stored.end(), stored.begin(),
                  stored.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(stored.size(), 8U)));
    std::vector<std::byte> result(512U + stored.size());
    ObjectPayloadWriter writer{result};
    constexpr std::string_view magic{"FSFSDEV3SPLX"};
    std::ranges::transform(magic, result.begin(), [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"SMPL"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    writer.be32(0x10, 512);
    writer.be32(0x14, 3);
    writer.be32(0x18, 0x7c);
    writer.be32(0x1c, static_cast<std::uint32_t>(stored.size()));
    writer.be32(0x20, static_cast<std::uint32_t>(stored.size()));
    writer.be16(0x28, static_cast<std::uint16_t>(audio.output_sample_rate));
    writer.be16(0x2a, 2);
    result[0x30] = std::byte{0x02};
    result[0x31] = std::byte{0xc0};
    auto name = ascii(spec.name, 16);
    if (!name)
        return std::unexpected{name.error()};
    std::ranges::copy(*name, result.begin() + 0x32);
    if (embedded_container_name.empty()) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "SMPL writer requires an embedded container name")};
    }
    auto container_name = ascii(embedded_container_name, 16);
    if (!container_name)
        return std::unexpected{container_name.error()};
    std::ranges::copy(*container_name, result.begin() + 0x54);
    writer.be32(0x78, reference_value);
    result[0x6c] = result[0x78];
    result[0x6d] = result[0x79];
    result[0x6e] = result[0x7a];
    writer.be16(0x7c, static_cast<std::uint16_t>(audio.output_sample_rate));
    result[0x7e] = static_cast<std::byte>(spec.root_key);
    result[0x7f] = static_cast<std::byte>(static_cast<std::uint8_t>(spec.fine_tune_cents));
    writer.be16(0x80, detail::sample_pitch_word(spec.root_key, spec.fine_tune_cents, audio.output_sample_rate));
    writer.be32(0x84, 0x30000000U | (static_cast<std::uint32_t>(spec.loop_mode) << 16U));
    writer.be32(0x8e, 0U);
    writer.be32(0x92, static_cast<std::uint32_t>(audio.output_frames));
    writer.be32(0x96, loop->start);
    writer.be32(0x9a, loop->length);
    if (auto written = writer.finish(); !written)
        return std::unexpected{written.error()};
    std::ranges::copy(stored, result.begin() + 512);
    return result;
}

struct LoadedWaveform {
    WaveformSpec spec;
    ImportedAudio audio;
    std::uint32_t reference_value{};
};

Result<void> write_program_link_bitmap(std::span<std::byte> bytes, std::size_t offset,
                                       const std::vector<std::uint8_t> &linked_programs) {
    if (bytes.size() < offset + 16U) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "Sample parameter block is too short for its Program-link bitmap")};
    }
    ByteWriter writer{bytes};
    for (const auto number : linked_programs) {
        if (number == 0U || number > 128U) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "linked Program number must be in the range 1..128")};
        }
        const auto word_offset = offset + ((number - 1U) / 32U) * 4U;
        const auto existing = (std::to_integer<std::uint32_t>(bytes[word_offset]) << 24U) |
                              (std::to_integer<std::uint32_t>(bytes[word_offset + 1U]) << 16U) |
                              (std::to_integer<std::uint32_t>(bytes[word_offset + 2U]) << 8U) |
                              std::to_integer<std::uint32_t>(bytes[word_offset + 3U]);
        const auto bit = std::uint32_t{1} << ((number - 1U) % 32U);
        if (auto written = writer.write_be32(word_offset, existing | bit); !written)
            return std::unexpected{written.error()};
    }
    return {};
}

Result<std::vector<std::byte>> serialize_sbnk(const SampleSpec &sample, const LoadedWaveform &left,
                                              const LoadedWaveform *right, bool sample_bank_member,
                                              const std::vector<std::uint8_t> &linked_programs) {
    if (auto valid = detail::validate_sample_parameters(sample.parameters); !valid)
        return std::unexpected{valid.error()};
    const auto root_key = sample.parameters.root_key.value_or(60U);
    const auto fine_tune = sample.parameters.fine_tune_cents.value_or(0);
    const auto key_low = sample.parameters.key_low.value_or(0U);
    const auto key_high = sample.parameters.key_high.value_or(127U);
    const auto loop_mode = sample.parameters.loop_mode.value_or(AudioSamplerLoopMode::forward_one_shot);
    const auto loop_start = sample.parameters.loop_start_frame.value_or(0U);
    const auto loop_length = sample.parameters.loop_length_frames.value_or(0U);
    const auto expand_detune = sample.parameters.expand_detune.value_or(0);
    const auto expand_dephase = sample.parameters.expand_dephase.value_or(0);
    const auto expand_width = sample.parameters.expand_width.value_or(63);
    const auto level = sample.parameters.level.value_or(100U);
    const auto velocity_low = sample.parameters.velocity_low.value_or(0U);
    const auto velocity_high = sample.parameters.velocity_high.value_or(127U);
    if (left.audio.output_frames > maximum_wave_data_frames_per_channel ||
        (right != nullptr && right->audio.output_frames > maximum_wave_data_frames_per_channel)) {
        return std::unexpected{make_error(ErrorCode::audio_wave_data_too_large, ErrorCategory::audio,
                                          "Sample references Wave Data beyond the A-series per-channel limit")};
    }
    if (left.audio.output_sample_rate == 0U ||
        left.audio.output_sample_rate > std::numeric_limits<std::uint16_t>::max() ||
        (right != nullptr && (right->audio.output_sample_rate == 0U ||
                              right->audio.output_sample_rate > std::numeric_limits<std::uint16_t>::max()))) {
        return std::unexpected{make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "Sample references an unencodable Wave Data sample rate")};
    }
    auto left_loop = loop_window(loop_mode, loop_start, loop_length, left.audio.output_frames);
    if (!left_loop)
        return std::unexpected{left_loop.error()};
    if (expand_detune < -7 || expand_detune > 7 || expand_dephase < -63 || expand_dephase > 63 || expand_width < -63 ||
        expand_width > 63 || (right != nullptr && (expand_detune != 0 || expand_dephase != 0))) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "Sample expand controls are invalid for its playback topology")};
    }
    std::optional<LoopWindow> right_loop;
    if (right != nullptr) {
        auto checked = loop_window(loop_mode, loop_start, loop_length, right->audio.output_frames);
        if (!checked)
            return std::unexpected{checked.error()};
        right_loop = *checked;
    }
    std::vector<std::byte> result(0x188);
    ObjectPayloadWriter writer{result};
    const auto put_text = [&](std::size_t offset, std::string_view value, std::size_t width) -> Result<void> {
        auto bytes = ascii(value, width);
        if (!bytes)
            return std::unexpected{bytes.error()};
        std::ranges::copy(*bytes, result.begin() + static_cast<std::ptrdiff_t>(offset));
        return {};
    };
    constexpr std::string_view magic{"FSFSDEV3SPLX"};
    std::ranges::transform(magic, result.begin(), [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"SBNK"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    constexpr std::array<std::byte, 16> header{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                               std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4},
                                               std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x34},
                                               std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x58}};
    std::ranges::copy(header, result.begin() + 0x10);
    result[0x30] = std::byte{0x10};
    result[0x31] = std::byte{0x0c};
    if (auto written = put_text(0x32, sample.name, 16); !written)
        return std::unexpected{written.error()};
    if (auto written = put_text(0x78, left.spec.name, 16); !written)
        return std::unexpected{written.error()};
    std::copy_n(result.begin() + 0x78, 3U, result.begin() + 0x6c);
    if (right != nullptr) {
        if (auto written = put_text(0x88, right->spec.name, 16); !written) {
            return std::unexpected{written.error()};
        }
    }
    writer.be32(0xa0, left.reference_value);
    writer.be32(0xa4, right == nullptr ? 0U : right->reference_value);
    constexpr std::array<std::byte, 24> controls{
        std::byte{0x4a}, std::byte{0x04}, std::byte{0x01}, std::byte{0x20}, std::byte{0x47}, std::byte{0x05},
        std::byte{0x01}, std::byte{0x20}, std::byte{0x49}, std::byte{0x0b}, std::byte{0x01}, std::byte{0xe0},
        std::byte{0x48}, std::byte{0x0c}, std::byte{0x01}, std::byte{0xe0}, std::byte{0},    std::byte{0},
        std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0}};
    std::ranges::copy(controls, result.begin() + 0xa8);
    const auto expanded = right == nullptr && (expand_detune != 0 || expand_dephase != 0);
    const auto topology_flags =
        (sample_bank_member ? 0x01U : 0U) | (right == nullptr ? 0x02U : 0U) | (expanded ? 0x04U : 0U);
    result[0xd0] = static_cast<std::byte>(topology_flags);
    result[0xd4] = std::byte{2};
    if (auto written = write_program_link_bitmap(result, 0xc0U, linked_programs); !written)
        return std::unexpected{written.error()};
    result[0xd6] = static_cast<std::byte>(root_key);
    result[0xdc] = static_cast<std::byte>(static_cast<std::uint8_t>(fine_tune));
    writer.be16(0xd8, static_cast<std::uint16_t>(left.audio.output_sample_rate));
    writer.be16(0xde, detail::sample_pitch_word(root_key, fine_tune, left.audio.output_sample_rate));
    if (right != nullptr) {
        result[0xd7] = static_cast<std::byte>(root_key);
        result[0xdd] = static_cast<std::byte>(static_cast<std::uint8_t>(fine_tune));
        writer.be16(0xda, static_cast<std::uint16_t>(right->audio.output_sample_rate));
        writer.be16(0xe0, detail::sample_pitch_word(root_key, fine_tune, right->audio.output_sample_rate));
    }
    result[0xe2] = static_cast<std::byte>(key_high);
    result[0xe3] = static_cast<std::byte>(key_low);
    result[0xe4] = std::byte{0x30};
    result[0xe5] = static_cast<std::byte>(loop_mode);
    writer.be16(0xe6, 9000);
    writer.be32(0xe8, 0U);
    writer.be32(0xec, 0U);
    writer.be32(0xf0, static_cast<std::uint32_t>(left.audio.output_frames));
    writer.be32(0xf8, left_loop->start);
    writer.be32(0x100, left_loop->length);
    writer.be32(0x15c, static_cast<std::uint32_t>(left.audio.output_frames));
    writer.be32(0x160, left_loop->start + left_loop->length);
    if (right != nullptr) {
        writer.be32(0xf4, static_cast<std::uint32_t>(right->audio.output_frames));
        writer.be32(0xfc, right_loop->start);
        writer.be32(0x104, right_loop->length);
    }
    const std::array<std::pair<std::size_t, std::uint8_t>, 32> defaults{
        {{0x109, 0},
         {0x10a, 127},
         {0x10b, 4},
         {0x10c, 0},
         {0x10d, 127},
         {0x10e, 0},
         {0x10f, 0},
         {0x110, 0},
         {0x111, 0},
         {0x112, static_cast<std::uint8_t>(expand_detune)},
         {0x113, static_cast<std::uint8_t>(expand_dephase)},
         {0x114, static_cast<std::uint8_t>(expand_width)},
         {0x115, 0},
         {0x116, level},
         {0x117, 0},
         {0x118, 0},
         {0x119, 0},
         {0x11a, velocity_high},
         {0x11b, velocity_low},
         {0x11c, 0},
         {0x11d, 127},
         {0x11e, 127},
         {0x11f, 127},
         {0x120, 0},
         {0x121, 0},
         {0x122, 26},
         {0x123, 64},
         {0x124, 10},
         {0x125, 0},
         {0x126, 127},
         {0x127, 127},
         {0x128, 127}}};
    for (const auto &[offset, value] : defaults)
        result[offset] = static_cast<std::byte>(value);
    result[0x131] = std::byte{127};
    result[0x132] = std::byte{127};
    result[0x133] = std::byte{127};
    result[0x13b] = std::byte{12};
    result[0x13c] = std::byte{127};
    result[0x13d] = std::byte{127};
    result[0x13e] = std::byte{126};
    result[0x141] = std::byte{127};
    result[0x146] = std::byte{1};
    result[0x147] = std::byte{39};
    result[0x149] = std::byte{1};
    constexpr std::array eq_coefficients{
        std::byte{0xc1}, std::byte{0xe0}, std::byte{0x1e}, std::byte{0x3a}, std::byte{0x20},
        std::byte{0x00}, std::byte{0x3e}, std::byte{0x20}, std::byte{0xe1}, std::byte{0xc6},
    };
    std::ranges::copy(eq_coefficients, result.begin() + 0x152);
    std::ranges::copy(controls, result.begin() + 0x164);
    result[0x17e] = std::byte{1};
    result[0x17f] = std::byte{127};
    result[0x181] = std::byte{127};
    result[0x183] = std::byte{90};
    result[0x184] = std::byte{90};
    if (auto applied = detail::apply_sample_parameters_to_block(std::span{result}.subspan(0xa8U), sample.parameters);
        !applied) {
        return std::unexpected{applied.error()};
    }
    if (auto written = writer.finish(); !written)
        return std::unexpected{written.error()};
    return result;
}

std::array<std::byte, 0xe0> default_sbac_sample_parameters() {
    std::array<std::byte, 0xe0> result{};
    const auto put_be16 = [&](std::size_t offset, std::uint16_t value) {
        result[offset] = static_cast<std::byte>(value >> 8U);
        result[offset + 1U] = static_cast<std::byte>(value);
    };
    constexpr std::array<std::byte, 16> controls{std::byte{0x4a}, std::byte{0x04}, std::byte{0x01}, std::byte{0x20},
                                                 std::byte{0x47}, std::byte{0x05}, std::byte{0x01}, std::byte{0x20},
                                                 std::byte{0x49}, std::byte{0x0b}, std::byte{0x01}, std::byte{0xe0},
                                                 std::byte{0x48}, std::byte{0x0c}, std::byte{0x01}, std::byte{0xe0}};
    constexpr std::array eq_coefficients{
        std::byte{0xc1}, std::byte{0xe0}, std::byte{0x1e}, std::byte{0x3a}, std::byte{0x20},
        std::byte{0x00}, std::byte{0x3e}, std::byte{0x20}, std::byte{0xe1}, std::byte{0xc6},
    };
    std::ranges::copy(controls, result.begin());
    result[0x2c] = std::byte{2};
    result[0x2e] = std::byte{60};
    result[0x2f] = std::byte{60};
    put_be16(0x30, 44'100U);
    put_be16(0x32, 44'100U);
    put_be16(0x36, detail::sample_pitch_word(60U, 0, 44'100U));
    put_be16(0x38, detail::sample_pitch_word(60U, 0, 44'100U));
    result[0x3a] = std::byte{127};
    result[0x3c] = std::byte{0x30};
    put_be16(0x3e, 9000U);
    constexpr std::array<std::pair<std::size_t, std::uint8_t>, 32> defaults{
        {{0x61, 0}, {0x62, 127}, {0x63, 4},  {0x64, 0},  {0x65, 127}, {0x66, 0},   {0x67, 0},   {0x68, 0},
         {0x69, 0}, {0x6a, 0},   {0x6b, 0},  {0x6c, 63}, {0x6d, 0},   {0x6e, 100}, {0x6f, 0},   {0x70, 0},
         {0x71, 0}, {0x72, 127}, {0x73, 0},  {0x74, 0},  {0x75, 127}, {0x76, 127}, {0x77, 127}, {0x78, 0},
         {0x79, 0}, {0x7a, 26},  {0x7b, 64}, {0x7c, 10}, {0x7d, 0},   {0x7e, 127}, {0x7f, 127}, {0x80, 127}}};
    for (const auto &[offset, value] : defaults)
        result[offset] = static_cast<std::byte>(value);
    result[0x89] = std::byte{127};
    result[0x8a] = std::byte{127};
    result[0x8b] = std::byte{127};
    result[0x93] = std::byte{12};
    result[0x94] = std::byte{127};
    result[0x95] = std::byte{127};
    result[0x96] = std::byte{126};
    result[0x97] = std::byte{8};
    result[0x98] = std::byte{127};
    result[0x99] = std::byte{127};
    result[0x9e] = std::byte{1};
    result[0x9f] = std::byte{39};
    result[0xa1] = std::byte{1};
    std::ranges::copy(eq_coefficients, result.begin() + 0xaa);
    std::ranges::copy(controls, result.begin() + 0xbc);
    result[0xd6] = std::byte{1};
    result[0xd7] = std::byte{127};
    result[0xd9] = std::byte{127};
    result[0xdb] = std::byte{90};
    result[0xdc] = std::byte{90};
    return result;
}

Result<void> apply_sbac_parameter_overrides(std::array<std::byte, 0xe0> &parameters,
                                            const SampleParameters &overrides) {
    if (auto applied = detail::apply_sample_parameters_to_block(parameters, overrides); !applied)
        return applied;
    const auto put_be16 = [&](std::size_t offset, std::uint16_t value) {
        parameters[offset] = static_cast<std::byte>(value >> 8U);
        parameters[offset + 1U] = static_cast<std::byte>(value);
    };
    if (overrides.root_key)
        parameters[0x2fU] = parameters[0x2eU];
    if (overrides.fine_tune_cents)
        parameters[0x35U] = parameters[0x34U];
    if (overrides.loop_start_frame)
        std::copy_n(parameters.begin() + 0x50U, 4U, parameters.begin() + 0x54U);
    if (overrides.loop_length_frames)
        std::copy_n(parameters.begin() + 0x58U, 4U, parameters.begin() + 0x5cU);
    if (overrides.root_key || overrides.fine_tune_cents) {
        const auto root_key = std::to_integer<std::uint8_t>(parameters[0x2eU]);
        const auto fine_tune = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(parameters[0x34U]));
        const auto pitch = detail::sample_pitch_word(root_key, fine_tune, 44'100U);
        put_be16(0x36U, pitch);
        put_be16(0x38U, pitch);
    }
    return {};
}

Result<std::vector<std::byte>> serialize_sbac(const SampleBankSpec &sample_bank,
                                              const std::map<std::string, SampleSpec> &samples,
                                              const std::vector<std::uint8_t> &linked_programs) {
    const std::set<std::string> unique_members{sample_bank.member_samples.begin(), sample_bank.member_samples.end()};
    if (sample_bank.member_samples.empty() || sample_bank.member_samples.size() > maximum_sample_bank_members ||
        unique_members.size() != sample_bank.member_samples.size()) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Sample Bank must contain 1..127 distinct Samples")};
    }
    if (sample_bank.parameter_overrides && !detail::has_sample_parameter_values(*sample_bank.parameter_overrides)) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "Sample Bank parameter overrides must not be empty")};
    }
    constexpr std::size_t minimum_record_size = 0x210U;
    constexpr std::size_t first_member_offset = 0x14cU;
    constexpr std::size_t member_stride = 0x14U;
    constexpr std::size_t trailing_parameter_bytes = 0x24U;
    const auto populated_record_size =
        first_member_offset + sample_bank.member_samples.size() * member_stride + trailing_parameter_bytes;
    std::vector<std::byte> result(std::max(minimum_record_size, populated_record_size));
    ObjectPayloadWriter writer{result};
    std::ranges::transform(std::string_view{"FSFSDEV3SPLX"}, result.begin(),
                           [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"SBAC"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    writer.be32(0x14, 4);
    writer.be32(0x18, static_cast<std::uint32_t>(result.size() - 0x54U));
    writer.be32(0x1c, static_cast<std::uint32_t>(result.size() - 0x30U));
    result[0x30] = std::byte{0x11};
    result[0x31] = std::byte{0x0c};
    auto name = ascii(sample_bank.name, 16);
    if (!name)
        return std::unexpected{name.error()};
    std::ranges::copy(*name, result.begin() + 0x32);
    auto parameters = default_sbac_sample_parameters();
    if (sample_bank.parameter_overrides) {
        if (auto applied = apply_sbac_parameter_overrides(parameters, *sample_bank.parameter_overrides); !applied)
            return std::unexpected{applied.error()};
    }
    if (auto written = write_program_link_bitmap(parameters, 0x18U, linked_programs); !written)
        return std::unexpected{written.error()};
    std::copy_n(parameters.begin(), 0xbcU, result.begin() + 0x78U);
    std::copy_n(parameters.begin() + 0xbcU, 0x24U, result.end() - 0x24U);
    result[0x144] = static_cast<std::byte>(sample_bank.member_samples.size());
    for (std::size_t index = 0; index < sample_bank.member_samples.size(); ++index) {
        const auto found = samples.find(sample_bank.member_samples[index]);
        if (found == samples.end())
            return std::unexpected{
                make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, "SBAC references an unknown SBNK")};
        auto member = ascii(found->second.name, 16);
        if (!member)
            return std::unexpected{member.error()};
        const auto offset = first_member_offset + index * member_stride;
        auto destination = std::span{result}.subspan(offset, member->size());
        std::ranges::copy(*member, destination.begin());
    }
    if (auto written = writer.finish(); !written)
        return std::unexpected{written.error()};
    return result;
}

Result<std::vector<std::byte>> serialize_prog(const ProgramSpec &program) {
    if (program.number == 0U || program.number > 128U || program.name.empty() || program.name.size() > 8U ||
        program.assignments.empty() || program.assignments.size() > maximum_program_assignments) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Program number or assignment count exceeds the encoded capacity")};
    }
    for (const auto &assignment : program.assignments) {
        if ((assignment.target_kind != "SBAC" && assignment.target_kind != "SBNK") || assignment.target_name.empty() ||
            (assignment.receive_mode == ProgramReceiveMode::midi_channel &&
             (assignment.receive_channel == 0U || assignment.receive_channel > 16U)) ||
            (assignment.receive_mode == ProgramReceiveMode::sample && assignment.receive_channel != 0U)) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "Program assignment cannot be represented by the object codec")};
        }
    }
    constexpr std::size_t assignment_offset = 0x120U;
    constexpr std::size_t assignment_stride = 0x38U;
    constexpr std::size_t payload_tail_size = 8U;
    const auto payload_size = assignment_offset + program.assignments.size() * assignment_stride + payload_tail_size;
    std::vector<std::byte> result(std::max<std::size_t>(0x390U, payload_size));
    ObjectPayloadWriter writer{result};
    std::ranges::transform(std::string_view{"FSFSDEV3SPLX"}, result.begin(),
                           [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"PROG"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    writer.be32(0x14, 4);
    writer.be32(0x18, static_cast<std::uint32_t>(result.size() - 0xe0U));
    writer.be32(0x1c, static_cast<std::uint32_t>(result.size() - 0x30U));
    result[0x30] = std::byte{0x14};
    result[0x31] = std::byte{0x0c};
    const auto object_name = std::format("{:03}", program.number);
    auto name = ascii(object_name, 16);
    if (!name)
        return std::unexpected{name.error()};
    std::ranges::copy(*name, result.begin() + 0x32);
    auto display = ascii(program.name, 8);
    if (!display)
        return std::unexpected{display.error()};
    std::ranges::copy(*display, result.begin() + 0x78);
    constexpr std::array<std::byte, 24> defaults{
        std::byte{0},    std::byte{5},    std::byte{0xff}, std::byte{0xff}, std::byte{0},    std::byte{0},
        std::byte{0},    std::byte{1},    std::byte{0x40}, std::byte{0},    std::byte{0x40}, std::byte{0x7f},
        std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0xfe}, std::byte{0},    std::byte{0x5a},
        std::byte{0x5a}, std::byte{0x27}, std::byte{0x78}, std::byte{0xff}, std::byte{0},    std::byte{2}};
    std::ranges::copy(defaults, result.begin() + 0x80);
    for (std::size_t index = 0; index < program.assignments.size(); ++index) {
        const auto &assignment = program.assignments[index];
        const auto offset = assignment_offset + index * assignment_stride;
        auto target = ascii(assignment.target_name, 16);
        if (!target)
            return std::unexpected{target.error()};
        std::ranges::copy(*target, result.begin() + static_cast<std::ptrdiff_t>(offset));
        result[offset + 0x14U] = assignment.target_kind == "SBAC" ? std::byte{0x11} : std::byte{0x10};
        result[offset + 0x15U] = assignment.receive_mode == ProgramReceiveMode::sample
                                     ? std::byte{0xff}
                                     : static_cast<std::byte>(assignment.receive_channel - 1U);
        result[offset + 0x1dU] = std::byte{0xff};
        result[offset + 0x1eU] = std::byte{0x7f};
        result[offset + 0x21U] = std::byte{0x7f};
        result[offset + 0x23U] = std::byte{0xff};
        result[offset + 0x24U] = std::byte{0xff};
        result[offset + 0x28U] = std::byte{0xff};
        result[offset + 0x2dU] = std::byte{0xff};
        result[offset + 0x30U] = std::byte{0xff};
        result[offset + 0x33U] = std::byte{1};
    }
    if (auto written = writer.finish(); !written)
        return std::unexpected{written.error()};
    return result;
}

} // namespace

Result<std::vector<std::byte>> detail::prepare_smpl_payload(const WaveformSpec &spec, const ImportedAudio &audio,
                                                            std::uint32_t reference_value,
                                                            std::string_view embedded_container_name) {
    return serialize_smpl(spec, audio, reference_value, embedded_container_name);
}

Result<std::vector<std::byte>> detail::prepare_sbnk_payload(const SampleSpec &spec, const PreparedWaveformMember &left,
                                                            const std::optional<PreparedWaveformMember> &right,
                                                            bool sample_bank_member,
                                                            const std::vector<std::uint8_t> &linked_programs) {
    ImportedAudio left_audio;
    left_audio.output_sample_rate = left.sample_rate;
    left_audio.output_frames = left.frame_count;
    WaveformSpec left_spec;
    left_spec.name = left.name;
    LoadedWaveform left_loaded{std::move(left_spec), std::move(left_audio), left.reference_value};
    std::optional<LoadedWaveform> right_loaded;
    if (right) {
        ImportedAudio right_audio;
        right_audio.output_sample_rate = right->sample_rate;
        right_audio.output_frames = right->frame_count;
        WaveformSpec right_spec;
        right_spec.name = right->name;
        right_loaded.emplace(LoadedWaveform{std::move(right_spec), std::move(right_audio), right->reference_value});
    }
    return serialize_sbnk(spec, left_loaded, right_loaded ? &*right_loaded : nullptr, sample_bank_member,
                          linked_programs);
}

Result<std::vector<std::byte>> detail::prepare_sbac_payload(const SampleBankSpec &sample_bank,
                                                            const std::map<std::string, SampleSpec> &samples,
                                                            const std::vector<std::uint8_t> &linked_programs) {
    return serialize_sbac(sample_bank, samples, linked_programs);
}

SampleSpec detail::apply_sample_bank_parameter_overrides(const SampleSpec &sample, const SampleParameters &overrides) {
    auto result = sample;
    detail::merge_sample_parameters(result.parameters, overrides);
    return result;
}

Result<std::vector<std::byte>> detail::prepare_prog_payload(const ProgramSpec &program) {
    return serialize_prog(program);
}

} // namespace axk
