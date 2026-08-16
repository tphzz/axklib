#include "axklib/tx16w.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/media.hpp"

namespace axk::tx16w {
namespace {

constexpr std::size_t header_size = 32U;
constexpr std::size_t setup_size = 1536U;
constexpr std::size_t voice_file_size = 8704U;
constexpr std::size_t performance_file_size = 5120U;
constexpr std::size_t record_header_size = 16U;
constexpr std::size_t voice_count = 32U;
constexpr std::size_t voice_stride = 0x92U;
constexpr std::size_t timbre_count = 64U;
constexpr std::size_t timbre_stride = 0x38U;
constexpr std::size_t timbre_table_offset = record_header_size + voice_count * voice_stride;
constexpr std::size_t wave_reference_count = 64U;
constexpr std::size_t wave_reference_stride = 16U;

Error parse_error(ErrorCode code, std::string message, std::string_view source_name = {}) {
    ErrorContext context;
    if (!source_name.empty())
        context.source_path = std::string{source_name};
    return make_error(code, ErrorCategory::object, std::move(message), std::move(context));
}

bool has_magic(std::span<const std::byte> bytes) {
    constexpr std::array magic{std::byte{'L'}, std::byte{'M'}, std::byte{'8'},
                               std::byte{'9'}, std::byte{'5'}, std::byte{'3'}};
    return bytes.size() >= magic.size() && std::ranges::equal(std::span{bytes}.first(magic.size()), magic);
}

Result<void> require_file(std::span<const std::byte> bytes, std::size_t minimum_size, std::string_view kind,
                          std::string_view source_name = {}) {
    if (bytes.size() < minimum_size) {
        return std::unexpected{parse_error(ErrorCode::container_truncated,
                                           std::string{kind} + " is shorter than its " + std::to_string(minimum_size) +
                                               "-byte native layout",
                                           source_name)};
    }
    if (!has_magic(bytes)) {
        return std::unexpected{parse_error(ErrorCode::object_malformed,
                                           std::string{kind} + " does not start with TX16W magic LM8953", source_name)};
    }
    return {};
}

Result<std::string> fixed_text(std::span<const std::byte> bytes, std::size_t offset, std::size_t width,
                               std::string_view field) {
    if (offset > bytes.size() || width > bytes.size() - offset) {
        return std::unexpected{
            parse_error(ErrorCode::container_truncated, std::string{field} + " extends beyond the TX16W file")};
    }
    std::string text;
    text.reserve(width);
    for (const auto value : bytes.subspan(offset, width)) {
        const auto character = std::to_integer<unsigned char>(value);
        if (character == 0U)
            break;
        if (character < 0x20U || character > 0x7eU)
            break;
        text.push_back(static_cast<char>(character));
    }
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos)
        text.clear();
    else if (first != 0U)
        text.erase(0U, first);
    return text;
}

std::uint32_t part_length(std::span<const std::byte> bytes, std::size_t offset) {
    return std::to_integer<std::uint32_t>(bytes[offset]) | (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           ((std::to_integer<std::uint32_t>(bytes[offset + 2U]) & 0x01U) << 16U);
}

struct SampleRateDecode {
    std::uint32_t rate{};
    SampleRateSource source{SampleRateSource::defaulted_33333};
    std::uint8_t native_rate_code{};
    std::uint8_t attack_marker{};
    std::uint8_t repeat_marker{};
};

SampleRateDecode sample_rate(std::span<const std::byte> bytes) {
    const auto native_rate_code = std::to_integer<std::uint8_t>(bytes[23U]);
    const auto attack_marker = std::to_integer<std::uint8_t>(bytes[26U]);
    const auto repeat_marker = std::to_integer<std::uint8_t>(bytes[29U]);
    const auto decoded = [&](std::uint32_t rate, SampleRateSource source) {
        return SampleRateDecode{rate, source, native_rate_code, attack_marker, repeat_marker};
    };
    switch (native_rate_code) {
    case 1U:
        return decoded(33333U, SampleRateSource::explicit_header);
    case 2U:
        return decoded(50000U, SampleRateSource::explicit_header);
    case 3U:
        return decoded(16667U, SampleRateSource::explicit_header);
    default:
        break;
    }
    if ((attack_marker & 0xfeU) == 0x06U && (repeat_marker & 0xfeU) == 0x52U)
        return decoded(33333U, SampleRateSource::length_markers);
    if ((attack_marker & 0xfeU) == 0x10U && (repeat_marker & 0xfeU) == 0x00U)
        return decoded(50000U, SampleRateSource::length_markers);
    if ((attack_marker & 0xfeU) == 0xf6U && (repeat_marker & 0xfeU) == 0x52U)
        return decoded(16667U, SampleRateSource::length_markers);
    return decoded(33333U, SampleRateSource::defaulted_33333);
}

struct SetupLayoutDecode {
    NativeSetupLayout layout{NativeSetupLayout::legacy};
    std::size_t wave_reference_offset{};
    bool write_protected{};
};

Result<SetupLayoutDecode> setup_layout(std::span<const std::byte> bytes, std::string_view source_name) {
    constexpr std::array version_0200{std::byte{'0'}, std::byte{'2'}, std::byte{'0'}, std::byte{'0'}};
    if (std::ranges::equal(bytes.subspan(7U, version_0200.size()), version_0200))
        return SetupLayoutDecode{NativeSetupLayout::version_0200, 0xf2U, false};
    const auto legacy_tail = bytes.subspan(7U, 9U);
    const auto protection = std::to_integer<std::uint8_t>(bytes[6U]);
    if (std::ranges::all_of(legacy_tail, [](std::byte value) { return value == std::byte{0}; }) && protection <= 1U)
        return SetupLayoutDecode{NativeSetupLayout::legacy, 0xf0U, protection == 1U};
    return std::unexpected{parse_error(ErrorCode::unsupported_profile,
                                       "TX16W Setup uses an unsupported native layout signature", source_name)};
}

std::int16_t expand_pcm(std::uint16_t packed) {
    auto signed_value = static_cast<std::int32_t>(packed);
    if ((packed & 0x0800U) != 0U)
        signed_value -= 0x1000;
    return static_cast<std::int16_t>(signed_value * 16);
}

std::string uppercase(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

bool same_file(const SourceFile &left, const SourceFile &right) { return left.bytes == right.bytes; }

Result<void> assign_member(const SourceFile *&destination, const SourceFile &source, std::string_view identity) {
    if (destination == nullptr) {
        destination = &source;
        return {};
    }
    if (same_file(*destination, source))
        return {};
    return std::unexpected{parse_error(ErrorCode::object_malformed,
                                       "TX16W disk set contains conflicting files for " + std::string{identity},
                                       source.name)};
}

struct FileIdentity {
    char kind{};
    std::string set_key;
    std::string set_name;
    std::uint8_t native_slot{};
};

std::optional<FileIdentity> identify_file(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    const auto base = name.substr(slash == std::string_view::npos ? 0U : slash + 1U);
    if (base.size() < 5U || base[base.size() - 4U] != '.' || !std::isdigit(static_cast<unsigned char>(base.back())) ||
        !std::isdigit(static_cast<unsigned char>(base[base.size() - 2U]))) {
        return std::nullopt;
    }
    const auto kind = static_cast<char>(std::toupper(static_cast<unsigned char>(base[base.size() - 3U])));
    const auto set_name = std::string{base.substr(0U, base.size() - 4U)};
    auto set_key = uppercase(set_name);
    set_key.push_back('|');
    set_key.append(base.substr(base.size() - 2U));
    const auto one_based_slot = static_cast<std::uint8_t>((base[base.size() - 2U] - '0') * 10 + (base.back() - '0'));
    const auto native_slot = one_based_slot == 0U ? std::uint8_t{0U} : static_cast<std::uint8_t>(one_based_slot - 1U);
    return FileIdentity{kind, std::move(set_key), set_name, native_slot};
}

struct NativeFileGroup {
    std::string name;
    const SourceFile *setup{};
    const SourceFile *voices{};
    const SourceFile *performances{};
};

} // namespace

Result<Wave> decode_wave(std::span<const std::byte> bytes, std::string source_name) {
    if (const auto required = require_file(bytes, header_size, "TX16W Wave", source_name); !required)
        return std::unexpected{required.error()};
    const auto rate = sample_rate(bytes);
    const auto format = std::to_integer<std::uint8_t>(bytes[22U]);
    if (format != 0x49U && format != 0xc9U) {
        return std::unexpected{parse_error(ErrorCode::audio_unsupported_format,
                                           "TX16W Wave has an unknown loop-mode encoding", source_name)};
    }

    Wave result;
    result.name = std::move(source_name);
    result.sample_rate = rate.rate;
    result.attack_frames = part_length(bytes, 24U);
    result.repeat_frames = part_length(bytes, 27U);
    result.looped = format == 0x49U;
    result.sample_rate_source = rate.source;
    result.native_rate_code = rate.native_rate_code;
    result.native_attack_rate_marker = rate.attack_marker;
    result.native_repeat_rate_marker = rate.repeat_marker;
    if (const auto identity = identify_file(result.name); identity && identity->kind == 'W')
        result.native_slot = identity->native_slot;
    const auto logical_frames = static_cast<std::uint64_t>(result.attack_frames) + result.repeat_frames;
    const auto packed_bytes = ((logical_frames + 1U) / 2U) * 3U;
    if (packed_bytes > bytes.size() - header_size) {
        return std::unexpected{parse_error(ErrorCode::container_truncated,
                                           "TX16W packed PCM payload is shorter than its declared logical length",
                                           result.name)};
    }
    result.pcm.reserve(static_cast<std::size_t>(logical_frames));
    const auto pcm_end = header_size + static_cast<std::size_t>(packed_bytes);
    for (std::size_t offset = header_size; offset < pcm_end; offset += 3U) {
        const auto first = std::to_integer<std::uint32_t>(bytes[offset]);
        const auto middle = std::to_integer<std::uint32_t>(bytes[offset + 1U]);
        const auto second = std::to_integer<std::uint32_t>(bytes[offset + 2U]);
        result.pcm.push_back(expand_pcm(static_cast<std::uint16_t>((first << 4U) | (middle >> 4U))));
        if (result.pcm.size() < logical_frames)
            result.pcm.push_back(expand_pcm(static_cast<std::uint16_t>((second << 4U) | (middle & 0x0fU))));
    }
    return result;
}

Result<NativeSetup> decode_native_setup(std::span<const std::byte> setup_bytes, std::span<const std::byte> voice_bytes,
                                        std::span<const std::byte> performance_bytes, std::string setup_name) {
    if (const auto required = require_file(setup_bytes, setup_size, "TX16W Setup", setup_name); !required)
        return std::unexpected{required.error()};
    if (const auto required = require_file(voice_bytes, voice_file_size, "TX16W Voice/Timbre file", setup_name);
        !required) {
        return std::unexpected{required.error()};
    }
    if (!performance_bytes.empty()) {
        if (const auto required =
                require_file(performance_bytes, performance_file_size, "TX16W Performance file", setup_name);
            !required) {
            return std::unexpected{required.error()};
        }
    }

    const auto layout = setup_layout(setup_bytes, setup_name);
    if (!layout)
        return std::unexpected{layout.error()};

    NativeSetup result;
    result.name = std::move(setup_name);
    result.layout = layout->layout;
    result.write_protected = layout->write_protected;
    for (std::size_t slot = 0U; slot < wave_reference_count; ++slot) {
        auto name = fixed_text(setup_bytes, layout->wave_reference_offset + slot * wave_reference_stride + 7U, 8U,
                               "TX16W Wave reference name");
        if (!name)
            return std::unexpected{name.error()};
        if (!name->empty())
            result.waves.push_back({static_cast<std::uint8_t>(slot), std::move(*name)});
    }
    if (!performance_bytes.empty()) {
        for (std::size_t slot = 0U; slot < voice_count; ++slot) {
            const auto base = record_header_size + slot * voice_stride;
            auto name = fixed_text(performance_bytes, base + 0x78U, 20U, "TX16W Performance name");
            if (!name)
                return std::unexpected{name.error()};
            if (name->empty())
                continue;
            Performance performance;
            performance.slot = static_cast<std::uint8_t>(slot);
            performance.name = std::move(*name);
            performance.native_header = {std::to_integer<std::uint8_t>(performance_bytes[base]),
                                         std::to_integer<std::uint8_t>(performance_bytes[base + 1U])};
            for (std::size_t index = 0U; index < performance.voices.size(); ++index) {
                performance.voices[index] = {
                    std::to_integer<std::uint8_t>(performance_bytes[base + 2U + index]),
                    std::to_integer<std::uint8_t>(performance_bytes[base + 18U + index]),
                    std::to_integer<std::uint8_t>(performance_bytes[base + 34U + index]),
                    std::to_integer<std::uint8_t>(performance_bytes[base + 50U + index]),
                    std::to_integer<std::uint8_t>(performance_bytes[base + 66U + index]),
                    static_cast<std::int8_t>(std::to_integer<std::uint8_t>(performance_bytes[base + 82U + index])),
                    static_cast<std::int8_t>(std::to_integer<std::uint8_t>(performance_bytes[base + 98U + index]))};
            }
            for (std::size_t index = 0U; index < performance.native_controls.size(); ++index)
                performance.native_controls[index] =
                    std::to_integer<std::uint8_t>(performance_bytes[base + 114U + index]);
            result.performances.push_back(std::move(performance));
        }
    }
    for (std::size_t slot = 0U; slot < voice_count; ++slot) {
        const auto base = record_header_size + slot * voice_stride;
        auto name = fixed_text(voice_bytes, base + 0x86U, 10U, "TX16W Voice name");
        if (!name)
            return std::unexpected{name.error()};
        if (name->empty())
            continue;
        Voice voice{static_cast<std::uint8_t>(slot), std::move(*name), {}};
        for (std::size_t region = 0U; region < 32U; ++region) {
            const auto offset = base + region * 4U;
            const auto low_key_number = std::to_integer<std::uint8_t>(voice_bytes[offset + 1U]);
            const auto high_key_number = std::to_integer<std::uint8_t>(voice_bytes[offset + 2U]);
            if (low_key_number == 0xffU)
                continue;
            const auto native_timbre_selector = std::to_integer<std::uint8_t>(voice_bytes[offset]);
            const auto timbre_slot = static_cast<std::uint8_t>(native_timbre_selector & 0x3fU);
            if (low_key_number > high_key_number) {
                return std::unexpected{parse_error(ErrorCode::object_malformed,
                                                   "TX16W Voice contains an invalid Timbre slot or key range")};
            }
            voice.regions.push_back({timbre_slot, low_key_number, high_key_number,
                                     std::to_integer<std::uint8_t>(voice_bytes[offset + 3U]),
                                     static_cast<std::uint8_t>(native_timbre_selector & 0xc0U)});
        }
        result.voices.push_back(std::move(voice));
    }
    for (std::size_t slot = 0U; slot < timbre_count; ++slot) {
        const auto base = timbre_table_offset + slot * timbre_stride;
        auto name = fixed_text(voice_bytes, base + 46U, 10U, "TX16W Timbre name");
        if (!name)
            return std::unexpected{name.error()};
        if (name->empty())
            continue;
        const auto wave_slot = std::to_integer<std::uint8_t>(voice_bytes[base]);
        const auto root_key_number = std::to_integer<std::uint8_t>(voice_bytes[base + 1U]);
        if (wave_slot >= wave_reference_count) {
            return std::unexpected{
                parse_error(ErrorCode::object_malformed, "TX16W Timbre contains an invalid Wave slot")};
        }
        result.timbres.push_back({static_cast<std::uint8_t>(slot), std::move(*name), wave_slot, root_key_number});
    }
    return result;
}

Result<Inspection> inspect_files(std::span<const SourceFile> files, const CancellationToken &cancellation) {
    Inspection result;
    std::map<std::string, NativeFileGroup, std::less<>> groups;
    for (const auto &file : files) {
        if (const auto cancelled = cancellation.check(); !cancelled)
            return std::unexpected{cancelled.error()};
        const auto identity = identify_file(file.name);
        if (!identity)
            continue;
        if (identity->kind == 'W') {
            const auto duplicate = std::ranges::find_if(
                result.waves, [&](const Wave &wave) { return uppercase(wave.name) == uppercase(file.name); });
            if (duplicate != result.waves.end()) {
                const auto existing = std::ranges::find_if(
                    files, [&](const SourceFile &candidate) { return candidate.name == duplicate->name; });
                if (existing != files.end() && same_file(*existing, file))
                    continue;
                return std::unexpected{parse_error(ErrorCode::object_malformed,
                                                   "TX16W disk set contains conflicting Wave files", file.name)};
            }
            auto wave = decode_wave(file.bytes, file.name);
            if (!wave)
                return std::unexpected{wave.error()};
            if (wave->sample_rate_source == SampleRateSource::defaulted_33333) {
                result.notices.push_back(
                    {ParseNoticeDisposition::defaulted, wave->name, "sample_rate_encoding",
                     "Unknown TX16W sample-rate encoding defaults to 33,333 Hz; verify pitch after import"});
            }
            wave->source_member = file.source_member;
            result.waves.push_back(std::move(*wave));
            continue;
        }
        auto &group = groups[identity->set_key];
        group.name = identity->set_name;
        if (identity->kind == 'S') {
            if (auto assigned = assign_member(group.setup, file, identity->set_key + " Setup"); !assigned)
                return std::unexpected{assigned.error()};
        } else if (identity->kind == 'V') {
            if (auto assigned = assign_member(group.voices, file, identity->set_key + " Voice"); !assigned)
                return std::unexpected{assigned.error()};
        } else if (identity->kind == 'U') {
            if (auto assigned = assign_member(group.performances, file, identity->set_key + " Performance");
                !assigned) {
                return std::unexpected{assigned.error()};
            }
        } else if (identity->kind == 'F')
            result.unsupported_files.push_back(file.name);
        else if (identity->kind == 'P' || identity->kind == 'O' || identity->kind == 'C' || identity->kind == 'R' ||
                 identity->kind == 'X')
            result.unsupported_files.push_back(file.name);
    }
    if (!result.unsupported_files.empty())
        result.profile = Profile::yamaha_native_with_auxiliary_files;
    for (const auto &[key, group] : groups) {
        static_cast<void>(key);
        if (group.setup == nullptr && group.voices == nullptr)
            continue;
        if (group.setup == nullptr || group.voices == nullptr) {
            return std::unexpected{parse_error(ErrorCode::object_missing,
                                               "TX16W native set requires matching Setup (.Snn) and Voice (.Vnn) files",
                                               group.name)};
        }
        const std::span<const std::byte> performance_bytes =
            group.performances == nullptr ? std::span<const std::byte>{} : std::span{group.performances->bytes};
        auto setup = decode_native_setup(group.setup->bytes, group.voices->bytes, performance_bytes, group.name);
        if (!setup)
            return std::unexpected{setup.error()};
        result.setups.push_back(std::move(*setup));
    }
    if (result.setups.empty() && result.waves.empty()) {
        return std::unexpected{parse_error(ErrorCode::container_unrecognized,
                                           "FAT image contains no Yamaha TX16W native Setup or Wave files")};
    }
    return result;
}

Result<Inspection> inspect_disk(const FatImage &disk, const CancellationToken &cancellation) {
    std::vector<SourceFile> files;
    files.reserve(disk.files().size());
    for (const auto &file : disk.files()) {
        if (const auto cancelled = cancellation.check(); !cancelled)
            return std::unexpected{cancelled.error()};
        const auto identity = identify_file(file.path);
        if (!identity)
            continue;
        auto bytes = disk.read_file(file, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        files.push_back({file.path, std::move(*bytes), disk.source_name()});
    }
    return inspect_files(files, cancellation);
}

} // namespace axk::tx16w
