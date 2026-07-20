#include "axklib/writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>

#include "axklib/catalog.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"
#include "writer_internal.hpp"

namespace axk {
namespace {

constexpr std::uint64_t cluster_size = 1024;

using detail::PreparedRecord;
using detail::RecordKind;

Result<void> validate_hds_image(const std::filesystem::path &path,
                                const std::vector<std::vector<PreparedRecord>> &records,
                                const CancellationToken &cancellation) {
    auto media = open_media(path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::sfs) {
        return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "written HDS reopened as the wrong container type")};
    }
    auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    std::size_t expected_objects{};
    for (const auto &partition : records) {
        expected_objects += static_cast<std::size_t>(std::ranges::count_if(
            partition, [](const PreparedRecord &record) { return record.kind == RecordKind::object; }));
    }
    if (catalog->objects.size() != expected_objects) {
        return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "written HDS object inventory failed reopen validation")};
    }
    return {};
}

void be16(std::span<std::byte> data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::byte>(value >> 8U);
    data[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void be32(std::span<std::byte> data, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        data[offset + index] = static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
    }
}

Result<std::vector<std::byte>> ascii(std::string_view value, std::size_t size, std::byte pad = std::byte{' '}) {
    if (value.size() > size || !std::ranges::all_of(value, [](unsigned char character) { return character < 0x80U; })) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "writer name does not fit its ASCII field")};
    }
    std::vector<std::byte> result(size, pad);
    std::ranges::transform(value, result.begin(), [](char character) { return static_cast<std::byte>(character); });
    return result;
}

Result<std::array<std::byte, 32>> directory_entry(std::string_view name, std::uint32_t link,
                                                  std::optional<std::size_t> fixed_width,
                                                  std::optional<std::uint32_t> directory_id, std::uint8_t marker) {
    auto name_bytes = ascii(name, fixed_width.value_or(name.size()));
    if (!name_bytes || name_bytes->size() + 1U > 24U)
        return std::unexpected{
            name_bytes ? make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, "directory name is too long")
                       : name_bytes.error()};
    std::array<std::byte, 32> result{};
    be16(result, 0, 0x20);
    be16(result, 2, static_cast<std::uint16_t>(name_bytes->size() + 1U));
    be32(result, 4, link);
    std::ranges::copy(*name_bytes, result.begin() + 8);
    if (name == ".") {
        result[10] = std::byte{0x4f};
        result[11] = std::byte{0x58};
        be32(result, 16, 0x0017821a);
        std::fill(result.begin() + 24, result.begin() + 28, std::byte{0xff});
        result[31] = static_cast<std::byte>(marker);
    } else if (name == "..") {
        if (!directory_id || *directory_id > 255U)
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "directory ID exceeds the writer profile")};
        result[11] = static_cast<std::byte>(*directory_id);
        be32(result, 16, 0x001783c8);
    }
    return result;
}

using Entry = std::tuple<std::string, std::uint32_t, std::optional<std::size_t>>;

Result<std::vector<std::byte>> directory_payload(std::uint32_t id, std::uint32_t parent,
                                                 const std::vector<Entry> &entries, std::uint8_t marker = 0) {
    std::vector<std::byte> result;
    const auto append = [&](const auto &entry) { result.insert(result.end(), entry.begin(), entry.end()); };
    auto dot = directory_entry(".", id, {}, id, marker);
    auto dotdot = directory_entry("..", parent, {}, id, marker);
    if (!dot)
        return std::unexpected{dot.error()};
    if (!dotdot)
        return std::unexpected{dotdot.error()};
    append(*dot);
    append(*dotdot);
    for (const auto &[name, link, width] : entries) {
        auto item = directory_entry(name, link, width, {}, marker);
        if (!item)
            return std::unexpected{item.error()};
        append(*item);
    }
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
                                              std::uint32_t link_id) {
    const auto pcm_bytes = audio.pcm_channels.size() == 1U ? audio.pcm_channels[0].size() : 0U;
    if (audio.output_frames > maximum_wave_data_frames_per_channel ||
        pcm_bytes > maximum_wave_data_pcm16_bytes_per_channel) {
        return std::unexpected{make_error(ErrorCode::audio_wave_data_too_large, ErrorCategory::audio,
                                          "Wave Data exceeds the 32 MiB per-channel A-series limit")};
    }
    if (audio.output_sample_width_bits != sampler_output_sample_width_bits || audio.pcm_channels.size() != 1U ||
        pcm_bytes % 2U != 0U || pcm_bytes / 2U != audio.output_frames ||
        audio.output_frames > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "SMPL writer requires bounded mono 16-bit PCM")};
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
    constexpr std::string_view magic{"FSFSDEV3SPLX"};
    std::ranges::transform(magic, result.begin(), [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"SMPL"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    be32(result, 0x10, 512);
    be32(result, 0x14, 3);
    be32(result, 0x18, 0x7c);
    be32(result, 0x1c, static_cast<std::uint32_t>(stored.size()));
    be32(result, 0x20, static_cast<std::uint32_t>(stored.size()));
    be16(result, 0x28, static_cast<std::uint16_t>(audio.output_sample_rate));
    be16(result, 0x2a, 2);
    result[0x30] = std::byte{0x02};
    result[0x31] = std::byte{0xc0};
    auto name = ascii(spec.name, 16);
    if (!name)
        return std::unexpected{name.error()};
    std::ranges::copy(*name, result.begin() + 0x32);
    constexpr std::array<std::byte, 8> identity{std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0x0a},
                                                std::byte{0x87}, std::byte{0x7c}, std::byte{0x01}, std::byte{0x54}};
    std::ranges::copy(identity, result.begin() + 0x42);
    be32(result, 0x68, 0x01443840);
    be32(result, 0x6c, link_id - 0xbaU);
    be32(result, 0x74, 0x01443840);
    be32(result, 0x78, link_id);
    be16(result, 0x7c, static_cast<std::uint16_t>(audio.output_sample_rate));
    result[0x7e] = static_cast<std::byte>(spec.root_key);
    be16(result, 0x80, pitch_word(spec.root_key, audio.output_sample_rate));
    be32(result, 0x84, 0x30010000);
    be32(result, 0x92, static_cast<std::uint32_t>(audio.output_frames));
    be32(result, 0x96, 0);
    be32(result, 0x9a, static_cast<std::uint32_t>(audio.output_frames));
    std::ranges::copy(stored, result.begin() + 512);
    return result;
}

struct LoadedWaveform {
    WaveformSpec spec;
    ImportedAudio audio;
    std::uint32_t link_id{};
};

Result<std::vector<std::byte>> serialize_sbnk(const SampleSpec &sample, const LoadedWaveform &left,
                                              const LoadedWaveform *right, bool sample_bank_member,
                                              const std::vector<std::uint8_t> &linked_programs) {
    if (left.audio.output_frames > maximum_wave_data_frames_per_channel ||
        (right != nullptr && right->audio.output_frames > maximum_wave_data_frames_per_channel)) {
        return std::unexpected{make_error(ErrorCode::audio_wave_data_too_large, ErrorCategory::audio,
                                          "Sample references Wave Data beyond the A-series per-channel limit")};
    }
    std::vector<std::byte> result(0x188);
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
    be32(result, 0x68, 0x01443c30);
    be32(result, 0x98, 0x01443c30);
    if (auto written = put_text(0x78, left.spec.name, 16); !written)
        return std::unexpected{written.error()};
    if (right != nullptr) {
        if (auto written = put_text(0x88, right->spec.name, 16); !written) {
            return std::unexpected{written.error()};
        }
    }
    be32(result, 0xa0, left.link_id);
    be32(result, 0xa4, right == nullptr ? 0U : right->link_id);
    constexpr std::array<std::byte, 16> member_defaults{
        std::byte{0x4a}, std::byte{0x04}, std::byte{0x01}, std::byte{0x20}, std::byte{0x47}, std::byte{0x05},
        std::byte{0x01}, std::byte{0x20}, std::byte{0x49}, std::byte{0x0b}, std::byte{0x01}, std::byte{0xe0},
        std::byte{0x48}, std::byte{0x0c}, std::byte{0x01}, std::byte{0xe0}};
    std::ranges::copy(member_defaults, result.begin() + 0xa8);
    result[0xd0] = static_cast<std::byte>(sample_bank_member ? 0x03U : 0x02U);
    result[0xd4] = std::byte{2};
    for (const auto number : linked_programs) {
        const auto offset = 0xc0U + ((number - 1U) / 32U) * 4U;
        const auto bit = static_cast<std::uint32_t>(1U << ((number - 1U) % 32U));
        const auto existing = (std::to_integer<std::uint32_t>(result[offset]) << 24U) |
                              (std::to_integer<std::uint32_t>(result[offset + 1U]) << 16U) |
                              (std::to_integer<std::uint32_t>(result[offset + 2U]) << 8U) |
                              std::to_integer<std::uint32_t>(result[offset + 3U]);
        be32(result, offset, existing | bit);
    }
    result[0xd6] = static_cast<std::byte>(sample.root_key);
    be16(result, 0xd8, static_cast<std::uint16_t>(left.audio.output_sample_rate));
    be16(result, 0xde, pitch_word(sample.root_key, left.audio.output_sample_rate));
    if (right != nullptr) {
        result[0xd7] = static_cast<std::byte>(sample.root_key);
        be16(result, 0xda, static_cast<std::uint16_t>(right->audio.output_sample_rate));
        be16(result, 0xe0, pitch_word(sample.root_key, right->audio.output_sample_rate));
    }
    result[0xe2] = static_cast<std::byte>(sample.key_high);
    result[0xe3] = static_cast<std::byte>(sample.key_low);
    result[0xe4] = std::byte{0x30};
    result[0xe5] = std::byte{1};
    be16(result, 0xe6, 9000);
    be32(result, 0xf0, static_cast<std::uint32_t>(left.audio.output_frames));
    be32(result, 0xf8, 0);
    be32(result, 0x100, static_cast<std::uint32_t>(left.audio.output_frames));
    if (right != nullptr) {
        be32(result, 0xf4, static_cast<std::uint32_t>(right->audio.output_frames));
        be32(result, 0xfc, 0);
        be32(result, 0x104, static_cast<std::uint32_t>(right->audio.output_frames));
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
    return result;
}

Result<std::vector<std::byte>> serialize_sbac(const SampleBankSpec &sample_bank,
                                              const std::map<std::string, SampleSpec> &samples) {
    std::vector<std::byte> result(0x210);
    std::ranges::transform(std::string_view{"FSFSDEV3SPLX"}, result.begin(),
                           [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"SBAC"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    be32(result, 0x14, 4);
    be32(result, 0x18, 0x1bc);
    be32(result, 0x1c, 0x1e0);
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
        std::ranges::copy(*member, result.begin() + static_cast<std::ptrdiff_t>(0x14cU + index * 0x14U));
    }
    return result;
}

Result<std::vector<std::byte>> serialize_prog(const ProgramSpec &program) {
    std::vector<std::byte> result(0x390);
    std::ranges::transform(std::string_view{"FSFSDEV3SPLX"}, result.begin(),
                           [](char value) { return static_cast<std::byte>(value); });
    std::ranges::transform(std::string_view{"PROG"}, result.begin() + 0x0c,
                           [](char value) { return static_cast<std::byte>(value); });
    be32(result, 0x14, 4);
    be32(result, 0x18, 0x2b0);
    be32(result, 0x1c, 0x360);
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
    return result;
}

} // namespace

Result<std::vector<std::byte>> detail::prepare_smpl_payload(const WaveformSpec &spec, const ImportedAudio &audio,
                                                            std::uint32_t link_id) {
    return serialize_smpl(spec, audio, link_id);
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
    LoadedWaveform left_loaded{std::move(left_spec), std::move(left_audio), left.link_id};
    std::optional<LoadedWaveform> right_loaded;
    if (right) {
        ImportedAudio right_audio;
        right_audio.output_sample_rate = right->sample_rate;
        right_audio.output_frames = right->frame_count;
        WaveformSpec right_spec;
        right_spec.name = right->name;
        right_loaded.emplace(LoadedWaveform{std::move(right_spec), std::move(right_audio), right->link_id});
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

Result<std::vector<PreparedRecord>> detail::prepare_partition_records(const PartitionSpec &partition,
                                                                      const PartitionGeometry &geometry,
                                                                      std::size_t partition_count,
                                                                      const CancellationToken &cancellation) {
    std::vector<PreparedRecord> records{{0, std::vector<std::byte>(32U * cluster_size), RecordKind::hidden},
                                        {2, {}, RecordKind::system}};
    std::vector<Entry> root{{"sfserrlog", 2, {}}, {"sfserram", 0, {}}};
    std::uint32_t next = 3;
    const auto marker = static_cast<std::uint8_t>(partition_count >= 3U ? geometry.index * 0x78U : 0U);
    constexpr std::array<std::string_view, 5> categories{"SMPL", "SBNK", "SBAC", "SEQU", "PROG"};
    for (const auto &volume : partition.volumes) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        if (volume.sample_banks.empty() != volume.programs.empty() ||
            volume.sample_banks.size() != volume.programs.size()) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "current SBAC/PROG profile requires one Sample Bank per Program")};
        }
        if (!volume.programs.empty()) {
            std::map<std::string, const SampleSpec *> sample_specs;
            std::map<std::string, const SampleBankSpec *> sample_bank_specs;
            for (const auto &sample : volume.samples)
                sample_specs.emplace(sample.name, &sample);
            for (const auto &sample_bank : volume.sample_banks)
                sample_bank_specs.emplace(sample_bank.name, &sample_bank);
            std::set<std::string> assigned_sample_banks;
            std::set<std::string> assigned_direct;
            for (const auto &program : volume.programs) {
                if (program.assignments.size() != 2U || program.assignments[0].target_kind != "SBAC" ||
                    program.assignments[0].receive_channel != 1U || program.assignments[1].target_kind != "SBNK" ||
                    program.assignments[1].receive_channel != 2U) {
                    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                      "Program profile requires SBAC channel 1 "
                                                      "then SBNK channel 2")};
                }
                const auto sample_bank = sample_bank_specs.find(program.assignments[0].target_name);
                const auto direct = sample_specs.find(program.assignments[1].target_name);
                if (sample_bank == sample_bank_specs.end() || direct == sample_specs.end() ||
                    !assigned_sample_banks.insert(sample_bank->first).second ||
                    !assigned_direct.insert(direct->first).second ||
                    std::ranges::contains(sample_bank->second->member_samples, direct->first)) {
                    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                      "Program targets must be unique, known, and separate")};
                }
                if (direct->second->right_waveform_id || direct->second->interleaved_audio_path) {
                    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                      "SBAC/PROG writer profile supports mono Samples only")};
                }
                for (const auto &member_name : sample_bank->second->member_samples) {
                    const auto member = sample_specs.find(member_name);
                    if (member == sample_specs.end() || member->second->right_waveform_id ||
                        member->second->interleaved_audio_path) {
                        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                          "SBAC/PROG writer profile supports mono "
                                                          "Samples only")};
                    }
                }
                if (sample_bank->second->member_samples.size() == 1U) {
                    const auto *member = sample_specs.at(sample_bank->second->member_samples[0]);
                    if (member->waveform_id != direct->second->waveform_id ||
                        member->root_key != direct->second->root_key || member->key_low != direct->second->key_low ||
                        member->key_high != direct->second->key_high || member->level != direct->second->level) {
                        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                          "one-member Sample Bank and direct Sample control "
                                                          "parameters must match")};
                    }
                }
            }
            if (assigned_sample_banks.size() != sample_bank_specs.size()) {
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "every Sample Bank must be assigned once")};
            }
        }
        const auto volume_id = next++;
        std::array<std::uint32_t, 5> category_ids{};
        for (auto &value : category_ids)
            value = next++;
        root.emplace_back(volume.name, volume_id, 16U);
        std::vector<Entry> volume_entries;
        for (std::size_t index = 0; index < categories.size(); ++index)
            volume_entries.emplace_back(std::string{categories[index]}, category_ids[index], std::nullopt);
        auto volume_data = directory_payload(volume_id, 1, volume_entries, marker);
        if (!volume_data)
            return std::unexpected{volume_data.error()};
        records.push_back({volume_id, std::move(*volume_data), RecordKind::directory,
                           static_cast<std::uint16_t>(2U + categories.size())});
        std::vector<Entry> smpl_entries;
        std::vector<Entry> sbnk_entries;
        std::vector<Entry> sbac_entries;
        std::vector<Entry> prog_entries;
        std::vector<PreparedRecord> objects;
        std::map<std::string, LoadedWaveform> loaded;
        for (std::size_t waveform_index = 0; waveform_index < volume.waveforms.size(); ++waveform_index) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto &spec = volume.waveforms[waveform_index];
            AudioImportOptions options;
            options.expected_channels = 1;
            options.target_sample_rate = spec.target_sample_rate;
            auto imported = import_sampler_audio(spec.path, options);
            if (!imported)
                return std::unexpected{imported.error()};
            const auto id = next++;
            const auto link_id = 0x016b1dbcU + static_cast<std::uint32_t>(waveform_index) * 0x100U;
            auto payload = serialize_smpl(spec, *imported, link_id);
            if (!payload)
                return std::unexpected{payload.error()};
            smpl_entries.emplace_back(spec.name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
            loaded.emplace(spec.id, LoadedWaveform{spec, std::move(*imported), link_id});
        }
        std::map<std::string, std::pair<std::string, std::string>> generated_members;
        std::set<std::string> banked_samples;
        for (const auto &sample_bank : volume.sample_banks) {
            banked_samples.insert(sample_bank.member_samples.begin(), sample_bank.member_samples.end());
        }
        std::map<std::string, std::vector<std::uint8_t>> linked_programs;
        for (const auto &program : volume.programs) {
            if (program.assignments.size() != 2U || program.assignments[0].target_kind != "SBAC" ||
                program.assignments[0].receive_channel != 1U || program.assignments[1].target_kind != "SBNK" ||
                program.assignments[1].receive_channel != 2U) {
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "Program profile requires SBAC channel 1 then SBNK channel "
                                                  "2")};
            }
            linked_programs[program.assignments[1].target_name].push_back(program.number);
        }
        std::map<std::string, SampleSpec> samples;
        for (const auto &sample : volume.samples) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            if (!sample.interleaved_audio_path)
                continue;
            AudioImportOptions options;
            options.expected_channels = 2;
            options.target_sample_rate = sample.target_sample_rate;
            auto imported = import_sampler_audio(*sample.interleaved_audio_path, options);
            if (!imported)
                return std::unexpected{imported.error()};
            const auto base = sample.name.substr(0, std::min<std::size_t>(sample.name.size(), 14U));
            const auto left_name = sample.left_waveform_name.value_or(base + "-L");
            const auto right_name = sample.right_waveform_name.value_or(base + "-R");
            const auto left_key = sample.name + "#left";
            const auto right_key = sample.name + "#right";
            const auto make_channel = [&](std::size_t channel) {
                auto audio = *imported;
                audio.source_channels = 1;
                audio.pcm_channels = {imported->pcm_channels[channel]};
                return audio;
            };
            const auto add_member = [&](std::string key, std::string name, std::size_t channel) -> Result<void> {
                WaveformSpec spec{key, std::move(name), *sample.interleaved_audio_path, sample.root_key,
                                  sample.target_sample_rate};
                const auto link_id = 0x016b1dbcU + static_cast<std::uint32_t>(loaded.size()) * 0x100U;
                auto audio = make_channel(channel);
                auto payload = serialize_smpl(spec, audio, link_id);
                if (!payload)
                    return std::unexpected{payload.error()};
                const auto id = next++;
                smpl_entries.emplace_back(spec.name, id, 16U);
                objects.push_back({id, std::move(*payload), RecordKind::object});
                loaded.emplace(key, LoadedWaveform{std::move(spec), std::move(audio), link_id});
                return {};
            };
            if (auto added = add_member(left_key, left_name, 0); !added)
                return std::unexpected{added.error()};
            if (auto added = add_member(right_key, right_name, 1); !added)
                return std::unexpected{added.error()};
            generated_members.emplace(sample.name, std::pair{left_key, right_key});
        }
        for (const auto &sample : volume.samples) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto generated = generated_members.find(sample.name);
            const auto left_key = sample.waveform_id                     ? *sample.waveform_id
                                  : generated == generated_members.end() ? std::string{}
                                                                         : generated->second.first;
            const auto right_key = sample.right_waveform_id               ? *sample.right_waveform_id
                                   : generated == generated_members.end() ? std::string{}
                                                                          : generated->second.second;
            const auto left = loaded.find(left_key);
            const auto right = right_key.empty() ? loaded.end() : loaded.find(right_key);
            if (left == loaded.end() || (!right_key.empty() && right == loaded.end()))
                return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                                  "SBNK references an unloaded waveform")};
            if (right != loaded.end() &&
                (left->second.audio.output_sample_rate != right->second.audio.output_sample_rate ||
                 left->second.audio.output_frames != right->second.audio.output_frames))
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "stereo SBNK members require matching rate and frame "
                                                  "count")};
            auto payload = serialize_sbnk(sample, left->second, right == loaded.end() ? nullptr : &right->second,
                                          banked_samples.contains(sample.name), linked_programs[sample.name]);
            if (!payload)
                return std::unexpected{payload.error()};
            const auto id = next++;
            sbnk_entries.emplace_back(sample.name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
            samples.emplace(sample.name, sample);
        }
        for (const auto &sample_bank : volume.sample_banks) {
            auto payload = serialize_sbac(sample_bank, samples);
            if (!payload)
                return std::unexpected{payload.error()};
            const auto id = next++;
            sbac_entries.emplace_back(sample_bank.name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
        }
        for (const auto &program : volume.programs) {
            auto payload = serialize_prog(program);
            if (!payload)
                return std::unexpected{payload.error()};
            const auto id = next++;
            const auto name = std::format("{:03}", program.number);
            prog_entries.emplace_back(name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
        }
        for (std::size_t category_index = 0; category_index < category_ids.size(); ++category_index) {
            const auto id = category_ids[category_index];
            auto category_data = directory_payload(id, volume_id,
                                                   category_index == 0U   ? smpl_entries
                                                   : category_index == 1U ? sbnk_entries
                                                   : category_index == 2U ? sbac_entries
                                                   : category_index == 4U ? prog_entries
                                                                          : std::vector<Entry>{},
                                                   marker);
            if (!category_data)
                return std::unexpected{category_data.error()};
            records.push_back({id, std::move(*category_data), RecordKind::directory, 2});
        }
        records.insert(records.end(), std::make_move_iterator(objects.begin()), std::make_move_iterator(objects.end()));
    }
    auto root_data = directory_payload(1, 1, root);
    if (!root_data)
        return std::unexpected{root_data.error()};
    records.push_back({1, std::move(*root_data), RecordKind::directory, static_cast<std::uint16_t>(root.size())});
    std::ranges::sort(records, {}, &PreparedRecord::id);
    std::uint64_t cluster = geometry.first_payload_cluster;
    for (auto &record : records) {
        if (record.kind == RecordKind::system)
            continue;
        record.cluster = static_cast<std::uint32_t>(cluster);
        record.clusters = static_cast<std::uint32_t>((record.payload.size() + cluster_size - 1U) / cluster_size);
        if (record.kind == RecordKind::directory || record.kind == RecordKind::object) {
            record.clusters = std::max(record.clusters, 2U);
        }
        cluster += record.clusters;
    }
    if (cluster >= geometry.cluster_count)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "partition does not have enough payload clusters")};
    return records;
}

namespace {

std::vector<std::byte> index_record(const PreparedRecord &record) {
    std::vector<std::byte> result(72);
    if (record.kind != RecordKind::system) {
        be16(result, 0, 1);
        be16(result, 4, static_cast<std::uint16_t>(record.clusters));
        be32(result, 6, static_cast<std::uint32_t>(record.payload.size()));
        be32(result, 0x0a, record.cluster);
        be32(result, 0x0e, record.clusters);
        be32(result, 0x12, static_cast<std::uint32_t>(record.payload.size()));
    }
    std::fill(result.begin() + 0x3a, result.begin() + 0x42, std::byte{0xff});
    result[0x42] = record.kind == RecordKind::object ? std::byte{0x9e} : std::byte{0x94};
    if (record.kind == RecordKind::directory) {
        result[0x43] = std::byte{'d'};
        result[0x44] = std::byte{'i'};
        result[0x45] = std::byte{'r'};
        be16(result, 0x46, record.tail);
    } else if (record.kind == RecordKind::object) {
        result[0x47] = std::byte{1};
    } else {
        result[0x47] = std::byte{1};
    }
    return result;
}

Result<void> write_at(std::fstream &output, std::uint64_t offset, std::span<const std::byte> bytes) {
    output.seekp(static_cast<std::streamoff>(offset));
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output)
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write fresh HDS data")};
    return {};
}

class TemporaryFileCleanup {
  public:
    explicit TemporaryFileCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFileCleanup() {
        if (active_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    TemporaryFileCleanup(const TemporaryFileCleanup &) = delete;
    TemporaryFileCleanup &operator=(const TemporaryFileCleanup &) = delete;
    void release() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

} // namespace

Result<WrittenImageLayout> write_hds_image(const HdsBuildManifest &manifest, const std::filesystem::path &output_path,
                                           bool overwrite, const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto geometries = plan_hds_geometry(manifest);
    if (!geometries)
        return std::unexpected{geometries.error()};
    if (!overwrite && std::filesystem::exists(output_path))
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "fresh HDS output already exists")};
    std::vector<std::vector<PreparedRecord>> all_records;
    for (std::size_t index = 0; index < geometries->size(); ++index) {
        auto records = detail::prepare_partition_records(manifest.partitions[index], (*geometries)[index],
                                                         geometries->size(), cancellation);
        if (!records)
            return std::unexpected{records.error()};
        all_records.push_back(std::move(*records));
    }
    std::error_code filesystem_error;
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    }
    if (filesystem_error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create HDS output directory")};
    const auto temporary = detail::reserve_temporary_file(output_path);
    if (!temporary)
        return std::unexpected{temporary.error()};
    TemporaryFileCleanup cleanup{*temporary};
    std::fstream output{*temporary, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc};
    if (!output)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create temporary HDS output")};
    output.seekp(static_cast<std::streamoff>(manifest.size_bytes - 1U));
    output.put('\0');
    std::vector<std::byte> superblock(512);
    const std::string_view magic{"YAMAHA_dev3"};
    std::ranges::transform(magic, superblock.begin(), [](char value) { return static_cast<std::byte>(value); });
    constexpr std::array<std::uint32_t, 7> residue{0xa1e00152, 0xa22c0000, 0x17,      0x09100000,
                                                   0x17,       0x09100000, 0x01000152};
    for (std::size_t index = 0; index < residue.size(); ++index)
        be32(superblock, 0x80 + index * 4U, residue[index]);
    be32(superblock, 0x9c, 512);
    be32(superblock, 0xa0, static_cast<std::uint32_t>(manifest.size_bytes / 512U));
    for (const auto &geometry : *geometries) {
        be32(superblock, 0xa8 + geometry.index * 8U, static_cast<std::uint32_t>(geometry.start_sector));
        be32(superblock, 0xac + geometry.index * 8U, static_cast<std::uint32_t>(geometry.filesystem_sector_count));
    }
    if (!write_at(output, 0, superblock) || !write_at(output, 512, superblock)) {
        output.close();
        std::filesystem::remove(*temporary);
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write HDS superblocks")};
    }
    WrittenImageLayout layout{output_path, manifest.size_bytes, {}, 0};
    for (std::size_t partition_index = 0; partition_index < geometries->size(); ++partition_index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto &geometry = (*geometries)[partition_index];
        const auto &records = all_records[partition_index];
        std::vector<std::byte> token(512);
        const auto token_text = std::format("{:08x}", 0xab432100U | geometry.index);
        std::ranges::transform(token_text, token.begin(), [](char value) { return static_cast<std::byte>(value); });
        const auto token_offset = geometry.index == 0 ? 1024U : (geometry.start_sector - 1U) * 512U;
        if (!write_at(output, token_offset, token))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write HDS transfer token")};
        std::vector<std::byte> header(1024);
        std::ranges::transform(magic, header.begin(), [](char value) { return static_cast<std::byte>(value); });
        auto name =
            ascii(manifest.partitions[partition_index].name, geometry.index > 0 && geometries->size() > 1 ? 15U : 16U);
        if (!name)
            return std::unexpected{name.error()};
        std::ranges::copy(*name, header.begin() + 0x40);
        if (geometry.index > 0 && geometries->size() > 1)
            header[0x4f] = static_cast<std::byte>('0' + geometry.index);
        be32(header, 0x80, 2);
        be32(header, 0x84, 200);
        std::fill(header.begin() + 0x88, header.begin() + 0x90, std::byte{0xff});
        be32(header, 0x90, static_cast<std::uint32_t>(geometry.cluster_count));
        be32(header, 0x94, 2);
        be32(header, 0x98, 2);
        be32(header, 0x9c, static_cast<std::uint32_t>(geometry.bitmap_cluster));
        be32(header, 0xa0, 5012);
        be32(header, 0xa4, static_cast<std::uint32_t>(geometry.directory_index_cluster));
        be32(header, 0xa8, 358);
        const auto dynamic = 0x0152a3fcU + geometry.index * 0x11U;
        const auto total_sectors = manifest.size_bytes / 512U;
        if (geometries->size() == 2U)
            header[0xaf] = static_cast<std::byte>(geometry.index);
        for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 17>{
                 {{0x104, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x114, geometry.index * 0x20U},
                  {0x118, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x11c, static_cast<std::uint32_t>(geometry.filesystem_sector_count)},
                  {0x134, dynamic},
                  {0x144, geometry.index == 0 ? 0x016f5bf0U : 0x015d6cc0U},
                  {0x14c, static_cast<std::uint32_t>(
                              geometries->size() >= 3U ? total_sectors - (geometries->size() - 2U) : total_sectors)},
                  {0x154, geometry.index},
                  {0x158, dynamic},
                  {0x15c, geometry.index * 0x20U},
                  {0x160, static_cast<std::uint32_t>(geometry.index * geometry.filesystem_sector_count)},
                  {0x164, dynamic},
                  {0x178, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x17c, static_cast<std::uint32_t>(geometry.filesystem_sector_count)},
                  {0x184, dynamic},
                  {0x194, geometries->size() >= 3U ? 0U : static_cast<std::uint32_t>(geometries->size())},
                  {0x1a8, geometry.index}}})
            be32(header, offset, value);
        const auto start = geometry.start_sector * 512U;
        if (!write_at(output, start, header) || !write_at(output, start + 1024U, header))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition headers")};
        std::vector<std::byte> bitmap(geometry.bitmap_cluster_count * cluster_size);
        std::vector<std::byte> index(((records.size() * 72U + 1023U) / 1024U) * 1024U);
        std::uint64_t allocated{};
        for (const auto &record : records) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            allocated += record.clusters;
            for (std::uint32_t cluster = record.cluster; cluster < record.cluster + record.clusters; ++cluster)
                bitmap[cluster / 8U] |= static_cast<std::byte>(0x80U >> (cluster % 8U));
            const auto offset = (record.id / 14U) * 1024U + (record.id % 14U) * 72U;
            const auto bytes = index_record(record);
            std::ranges::copy(bytes, index.begin() + static_cast<std::ptrdiff_t>(offset));
            if (!record.payload.empty() &&
                !write_at(output, (geometry.start_sector + record.cluster * 2U) * 512U, record.payload))
                return std::unexpected{
                    make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition payload")};
        }
        if (!write_at(output, start + 2048U, std::span{bitmap}.first(512)) ||
            !write_at(output, (geometry.start_sector + geometry.bitmap_cluster * 2U) * 512U, bitmap) ||
            !write_at(output, (geometry.start_sector + geometry.directory_index_cluster * 2U) * 512U, index))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition allocation data")};
        const auto free_clusters = geometry.cluster_count - geometry.first_payload_cluster - allocated;
        layout.partitions.push_back({geometry, manifest.partitions[partition_index].name, allocated, free_clusters,
                                     free_clusters * 1024U, (free_clusters * 1024U) / 1024U});
    }
    if (const auto check = cancellation.check(); !check) {
        return std::unexpected{check.error()};
    }
    output.flush();
    output.close();
    if (auto flushed = detail::flush_file_to_disk(*temporary); !flushed)
        return std::unexpected{flushed.error()};
    if (auto validated = validate_hds_image(*temporary, all_records, cancellation); !validated)
        return std::unexpected{validated.error()};
    if (auto published = detail::publish_temporary_file(*temporary, output_path, overwrite); !published)
        return std::unexpected{published.error()};
    cleanup.release();
    const auto &last = geometries->back();
    layout.unused_tail_sectors = manifest.size_bytes / 512U - (last.start_sector + last.filesystem_sector_count);
    return layout;
}

Result<HdsBuildPlanSummary> plan_hds_build(const HdsBuildManifest &manifest, const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto geometries = plan_hds_geometry(manifest);
    if (!geometries)
        return std::unexpected{geometries.error()};
    std::size_t object_count{};
    for (std::size_t index = 0; index < geometries->size(); ++index) {
        auto records = detail::prepare_partition_records(manifest.partitions[index], (*geometries)[index],
                                                         geometries->size(), cancellation);
        if (!records)
            return std::unexpected{records.error()};
        object_count += static_cast<std::size_t>(std::ranges::count_if(
            *records, [](const PreparedRecord &record) { return record.kind == RecordKind::object; }));
    }
    return HdsBuildPlanSummary{manifest.size_bytes, geometries->size(), object_count, std::move(*geometries)};
}

} // namespace axk
