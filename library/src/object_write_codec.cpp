#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

std::uint16_t pitch_word(std::uint8_t root_key, std::uint32_t sample_rate) {
    constexpr std::array<std::uint16_t, 12> fractions{0x000, 0x055, 0x0ab, 0x100, 0x155, 0x1ab,
                                                      0x200, 0x255, 0x2ab, 0x300, 0x355, 0x3ab};
    const auto root = root_key == 0U ? 0x03ab : ((root_key - 1U) / 12U) * 1024U + fractions[(root_key - 1U) % 12U];
    const auto rate = static_cast<int>(std::log(static_cast<double>(sample_rate) / 44'100.0) * 1477.3197);
    return static_cast<std::uint16_t>(static_cast<int>(root) - rate);
}

Result<std::vector<std::byte>> serialize_smpl(const WaveformSpec &spec, const ImportedAudio &audio,
                                              std::uint32_t reference_value) {
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
    if (reference_value < 0xbaU) {
        return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "SMPL reference value is below the encoded relocation base")};
    }
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
    constexpr std::array<std::byte, 8> identity{std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0x0a},
                                                std::byte{0x87}, std::byte{0x7c}, std::byte{0x01}, std::byte{0x54}};
    std::ranges::copy(identity, result.begin() + 0x42);
    writer.be32(0x68, 0x01443840);
    writer.be32(0x6c, reference_value - 0xbaU);
    writer.be32(0x74, 0x01443840);
    writer.be32(0x78, reference_value);
    writer.be16(0x7c, static_cast<std::uint16_t>(audio.output_sample_rate));
    result[0x7e] = static_cast<std::byte>(spec.root_key);
    writer.be16(0x80, pitch_word(spec.root_key, audio.output_sample_rate));
    writer.be32(0x84, 0x30010000);
    writer.be32(0x92, static_cast<std::uint32_t>(audio.output_frames));
    writer.be32(0x96, 0);
    writer.be32(0x9a, static_cast<std::uint32_t>(audio.output_frames));
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

Result<std::vector<std::byte>> serialize_sbnk(const SampleSpec &sample, const LoadedWaveform &left,
                                              const LoadedWaveform *right, bool sample_bank_member,
                                              const std::vector<std::uint8_t> &linked_programs) {
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
    constexpr std::array<std::byte, 7> suffix{std::byte{0xb8}, std::byte{0},    std::byte{0x0a}, std::byte{0xf6},
                                              std::byte{0x7a}, std::byte{0x01}, std::byte{0x54}};
    std::ranges::copy(suffix, result.begin() + 0x43);
    std::fill(result.begin() + 0x50, result.begin() + 0x68, std::byte{' '});
    writer.be32(0x68, 0x01443c30);
    writer.be32(0x98, 0x01443c30);
    if (auto written = put_text(0x78, left.spec.name, 16); !written)
        return std::unexpected{written.error()};
    if (right != nullptr) {
        if (auto written = put_text(0x88, right->spec.name, 16); !written) {
            return std::unexpected{written.error()};
        }
    }
    writer.be32(0xa0, left.reference_value);
    writer.be32(0xa4, right == nullptr ? 0U : right->reference_value);
    constexpr std::array<std::byte, 16> member_defaults{
        std::byte{0x4a}, std::byte{0x04}, std::byte{0x01}, std::byte{0x20}, std::byte{0x47}, std::byte{0x05},
        std::byte{0x01}, std::byte{0x20}, std::byte{0x49}, std::byte{0x0b}, std::byte{0x01}, std::byte{0xe0},
        std::byte{0x48}, std::byte{0x0c}, std::byte{0x01}, std::byte{0xe0}};
    std::ranges::copy(member_defaults, result.begin() + 0xa8);
    result[0xd0] = static_cast<std::byte>(sample_bank_member ? 0x03U : 0x02U);
    result[0xd4] = std::byte{2};
    for (const auto number : linked_programs) {
        if (number == 0U || number > 128U) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "linked Program number must be in the range 1..128")};
        }
        const auto offset = 0xc0U + ((number - 1U) / 32U) * 4U;
        const auto bit = static_cast<std::uint32_t>(1U << ((number - 1U) % 32U));
        const auto existing = (std::to_integer<std::uint32_t>(result[offset]) << 24U) |
                              (std::to_integer<std::uint32_t>(result[offset + 1U]) << 16U) |
                              (std::to_integer<std::uint32_t>(result[offset + 2U]) << 8U) |
                              std::to_integer<std::uint32_t>(result[offset + 3U]);
        writer.be32(offset, existing | bit);
    }
    result[0xd6] = static_cast<std::byte>(sample.root_key);
    writer.be16(0xd8, static_cast<std::uint16_t>(left.audio.output_sample_rate));
    writer.be16(0xde, pitch_word(sample.root_key, left.audio.output_sample_rate));
    if (right != nullptr) {
        result[0xd7] = static_cast<std::byte>(sample.root_key);
        writer.be16(0xda, static_cast<std::uint16_t>(right->audio.output_sample_rate));
        writer.be16(0xe0, pitch_word(sample.root_key, right->audio.output_sample_rate));
    }
    result[0xe2] = static_cast<std::byte>(sample.key_high);
    result[0xe3] = static_cast<std::byte>(sample.key_low);
    result[0xe4] = std::byte{0x30};
    result[0xe5] = std::byte{1};
    writer.be16(0xe6, 9000);
    writer.be32(0xf0, static_cast<std::uint32_t>(left.audio.output_frames));
    writer.be32(0xf8, 0);
    writer.be32(0x100, static_cast<std::uint32_t>(left.audio.output_frames));
    if (right != nullptr) {
        writer.be32(0xf4, static_cast<std::uint32_t>(right->audio.output_frames));
        writer.be32(0xfc, 0);
        writer.be32(0x104, static_cast<std::uint32_t>(right->audio.output_frames));
    }
    const std::array<std::pair<std::size_t, std::uint8_t>, 32> defaults{
        {{0x109, 0},   {0x10a, 127}, {0x10b, 4},   {0x10c, 0},   {0x10d, 127}, {0x10e, 0},  {0x10f, 0},
         {0x110, 0},   {0x111, 0},   {0x112, 0},   {0x113, 0},   {0x114, 63},  {0x115, 0},  {0x116, sample.level},
         {0x117, 0},   {0x118, 0},   {0x119, 0},   {0x11a, 127}, {0x11b, 0},   {0x11c, 0},  {0x11d, 127},
         {0x11e, 127}, {0x11f, 127}, {0x120, 0},   {0x121, 0},   {0x122, 26},  {0x123, 64}, {0x124, 10},
         {0x125, 0},   {0x126, 127}, {0x127, 127}, {0x128, 127}}};
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
    constexpr std::array<std::byte, 5> playback{std::byte{0xc1}, std::byte{0xe0}, std::byte{0x1e}, std::byte{0x3a},
                                                std::byte{0x20}};
    constexpr std::array<std::byte, 4> tone{std::byte{0x3e}, std::byte{0x20}, std::byte{0xe1}, std::byte{0xc6}};
    std::ranges::copy(playback, result.begin() + 0x152);
    std::ranges::copy(tone, result.begin() + 0x158);
    constexpr std::array<std::byte, 24> controls{
        std::byte{74}, std::byte{4},  std::byte{1},  std::byte{32},   std::byte{71}, std::byte{5},
        std::byte{1},  std::byte{32}, std::byte{73}, std::byte{11},   std::byte{1},  std::byte{0xe0},
        std::byte{72}, std::byte{12}, std::byte{1},  std::byte{0xe0}, std::byte{0},  std::byte{0},
        std::byte{0},  std::byte{0},  std::byte{0},  std::byte{0},    std::byte{0},  std::byte{0}};
    std::ranges::copy(controls, result.begin() + 0x164);
    result[0x17e] = std::byte{1};
    result[0x17f] = std::byte{127};
    result[0x181] = std::byte{127};
    result[0x183] = std::byte{90};
    result[0x184] = std::byte{90};
    if (auto written = writer.finish(); !written)
        return std::unexpected{written.error()};
    return result;
}

Result<std::vector<std::byte>> serialize_sbac(const SampleBankSpec &sample_bank,
                                              const std::map<std::string, SampleSpec> &samples) {
    const std::set<std::string> unique_members{sample_bank.member_samples.begin(), sample_bank.member_samples.end()};
    if (sample_bank.member_samples.empty() || sample_bank.member_samples.size() > 3U ||
        unique_members.size() != sample_bank.member_samples.size()) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Sample Bank must contain 1..3 distinct Samples")};
    }
    std::vector<std::byte> result(0x210);
    ObjectPayloadWriter writer{result};
    std::ranges::transform(std::string_view{"FSFSDEV3SPLX"}, result.begin(),
                           [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"SBAC"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    writer.be32(0x14, 4);
    writer.be32(0x18, 0x1bc);
    writer.be32(0x1c, 0x1e0);
    result[0x30] = std::byte{0x11};
    result[0x31] = std::byte{0x0c};
    auto name = ascii(sample_bank.name, 16);
    if (!name)
        return std::unexpected{name.error()};
    std::ranges::copy(*name, result.begin() + 0x32);
    result[0x144] = static_cast<std::byte>(sample_bank.member_samples.size());
    for (std::size_t index = 0; index < sample_bank.member_samples.size(); ++index) {
        const auto found = samples.find(sample_bank.member_samples[index]);
        if (found == samples.end())
            return std::unexpected{
                make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, "SBAC references an unknown SBNK")};
        auto member = ascii(found->second.name, 16);
        if (!member)
            return std::unexpected{member.error()};
        const auto offset = 0x14cU + index * 0x14U;
        auto destination = std::span{result}.subspan(offset, member->size());
        std::ranges::copy(*member, destination.begin());
    }
    if (auto written = writer.finish(); !written)
        return std::unexpected{written.error()};
    return result;
}

Result<std::vector<std::byte>> serialize_prog(const ProgramSpec &program) {
    constexpr std::size_t maximum_assignments = 11U;
    if (program.number == 0U || program.number > 128U || program.assignments.size() > maximum_assignments) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Program number or assignment count exceeds the encoded capacity")};
    }
    for (const auto &assignment : program.assignments) {
        if ((assignment.target_kind != "SBAC" && assignment.target_kind != "SBNK") || assignment.target_name.empty() ||
            assignment.receive_channel == 0U || assignment.receive_channel > 16U) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "Program assignment cannot be represented by the object codec")};
        }
    }
    std::vector<std::byte> result(0x390);
    ObjectPayloadWriter writer{result};
    std::ranges::transform(std::string_view{"FSFSDEV3SPLX"}, result.begin(),
                           [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"PROG"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    writer.be32(0x14, 4);
    writer.be32(0x18, 0x2b0);
    writer.be32(0x1c, 0x360);
    result[0x30] = std::byte{0x14};
    result[0x31] = std::byte{0x0c};
    const auto object_name = std::format("{:03}", program.number);
    auto name = ascii(object_name, 16);
    if (!name)
        return std::unexpected{name.error()};
    std::ranges::copy(*name, result.begin() + 0x32);
    auto display = ascii("Pgm " + object_name, 8);
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
        const auto offset = 0x120U + index * 0x38U;
        auto target = ascii(assignment.target_name, 16);
        if (!target)
            return std::unexpected{target.error()};
        std::ranges::copy(*target, result.begin() + static_cast<std::ptrdiff_t>(offset));
        result[offset + 0x14U] = assignment.target_kind == "SBAC" ? std::byte{0x11} : std::byte{0x10};
        result[offset + 0x15U] = static_cast<std::byte>(assignment.receive_channel - 1U);
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
                                                            std::uint32_t reference_value) {
    return serialize_smpl(spec, audio, reference_value);
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
                                                            const std::map<std::string, SampleSpec> &samples) {
    return serialize_sbac(sample_bank, samples);
}

Result<std::vector<std::byte>> detail::prepare_prog_payload(const ProgramSpec &program) {
    return serialize_prog(program);
}

} // namespace axk
