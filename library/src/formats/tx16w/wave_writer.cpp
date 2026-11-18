#include "axklib/formats/tx16w/wave_writer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"

namespace axk::tx16w {
namespace {

constexpr std::size_t header_size = 32;
constexpr std::size_t sector_size = 512;

struct RateMarkers {
    std::uint8_t code;
    std::uint8_t attack;
    std::uint8_t repeat;
};

Result<RateMarkers> rate_markers(const WaveEncoding &encoding) {
    if (encoding.sample_rate == 33333U)
        return RateMarkers{1, 0x06, 0x52};
    if (encoding.sample_rate == 50000U && encoding.looped)
        return RateMarkers{2, 0x10, 0x00};
    return std::unexpected(make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                      "Unsupported TX16W Wave writing rate and loop mode"));
}

void write_part(std::span<std::byte, 3> dest, std::uint32_t frames, std::uint8_t rate_marker) {
    dest[0] = static_cast<std::byte>(frames & 0xffU);
    dest[1] = static_cast<std::byte>((frames >> 8U) & 0xffU);
    dest[2] = static_cast<std::byte>(rate_marker | (frames >> 16U));
}

void write_header(std::span<std::byte, header_size> header, const WaveEncoding &e, RateMarkers markers) {
    constexpr std::string_view magic = "LM8953";
    std::ranges::transform(magic, header.begin(), [](char c) { return static_cast<std::byte>(c); });
    std::ranges::fill(header.subspan<18, 4>(), std::byte{0x7f});
    header[22] = e.looped ? std::byte{0x49} : std::byte{0xc9};
    header[23] = static_cast<std::byte>(markers.code);
    write_part(header.subspan<24, 3>(), e.attack_frames, markers.attack);
    write_part(header.subspan<27, 3>(), e.repeat_frames, markers.repeat);
}

} // namespace

WaveFileSize wave_file_size(std::uint32_t frames) noexcept {
    const auto used = header_size + 3ULL * ((static_cast<std::uint64_t>(frames) + 1U) / 2U);
    return {used, ((used + sector_size - 1U) / sector_size) * sector_size};
}

Result<std::vector<std::byte>> encode_wave(const WaveEncoding &encoding, std::span<const std::int16_t> pcm) {
    const auto markers = rate_markers(encoding);
    if (!markers)
        return std::unexpected(markers.error());
    const auto valid_part = [](std::uint32_t frames) { return frames >= 64U && frames <= 0x1ffffU; };
    if (!valid_part(encoding.attack_frames) || !valid_part(encoding.repeat_frames))
        return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::audio,
                                          "TX16W Wave writer requires 64 to 131071 frames per part"));
    if (pcm.size() != encoding.attack_frames + encoding.repeat_frames)
        return std::unexpected(
            make_error(ErrorCode::invalid_argument, ErrorCategory::audio,
                       "TX16W Wave attack and repeat lengths must cover exactly the supplied frames"));
    if (std::ranges::any_of(pcm, [](std::int16_t frame) { return frame % 16 != 0; }))
        return std::unexpected(make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "TX16W Wave frames must be exactly representable at 12-bit precision"));

    const auto size = wave_file_size(encoding.attack_frames + encoding.repeat_frames);
    std::vector<std::byte> file(static_cast<std::size_t>(size.padded_bytes));
    write_header(std::span{file}.first<header_size>(), encoding, *markers);
    for (std::size_t i = 0, out = header_size; i < pcm.size(); i += 2, out += 3) {
        const auto a = static_cast<std::uint16_t>(pcm[i]);
        const auto b = i + 1 < pcm.size() ? static_cast<std::uint16_t>(pcm[i + 1]) : std::uint16_t{};
        file[out] = static_cast<std::byte>(a >> 8U);
        file[out + 1] = static_cast<std::byte>((a & 0xf0U) | ((b >> 4U) & 0x0fU));
        file[out + 2] = static_cast<std::byte>(b >> 8U);
    }
    return file;
}

} // namespace axk::tx16w
