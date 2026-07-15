#include "axklib/wav_stream.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

#include "axklib/utf8.hpp"

namespace axk::audio_internal {
namespace {

constexpr std::size_t wav_header_size = 44U;
constexpr std::size_t stream_buffer_size = 64U * 1024U;

struct WavLayout {
    std::uint16_t channels{};
    std::uint16_t width{};
    std::uint32_t sample_rate{};
    std::size_t data_size{};
};

Result<void> validate_waveform(const Waveform &waveform) {
    const auto channels = waveform.format.channels;
    const auto width = waveform.format.sample_width_bytes;
    if ((channels != 1U && channels != 2U) || (width != 1U && width != 2U) || waveform.format.sample_rate == 0U) {
        return std::unexpected{make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "waveform format must be mono/stereo, 8/16-bit PCM with a non-zero "
                                          "sample rate")};
    }
    const auto block = static_cast<std::size_t>(channels) * width;
    if (waveform.pcm.size() % block != 0U || waveform.frame_count != waveform.pcm.size() / block) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::audio,
                                          "waveform frame count and PCM byte length are inconsistent")};
    }
    return {};
}

Result<WavLayout> layout(const WavSource &source) {
    WavLayout result;
    if (source.physical != nullptr && source.left == nullptr && source.right == nullptr) {
        if (const auto valid = validate_waveform(*source.physical); !valid)
            return std::unexpected{valid.error()};
        result = {source.physical->format.channels, source.physical->format.sample_width_bytes,
                  source.physical->format.sample_rate, source.physical->pcm.size()};
    } else if (source.physical == nullptr && source.left != nullptr && source.right != nullptr) {
        if (const auto valid = validate_waveform(*source.left); !valid)
            return std::unexpected{valid.error()};
        if (const auto valid = validate_waveform(*source.right); !valid)
            return std::unexpected{valid.error()};
        const auto decision = stereo_render_decision(*source.left, *source.right);
        if (!decision.renderable) {
            return std::unexpected{
                make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio, decision.reason_code)};
        }
        const auto width = source.left->format.sample_width_bytes;
        if (decision.output_frame_count > std::numeric_limits<std::size_t>::max() / (2U * width)) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::audio,
                                              "rendered stereo PCM exceeds the addressable size")};
        }
        result = {2U, width, source.left->format.sample_rate,
                  static_cast<std::size_t>(decision.output_frame_count) * 2U * width};
    } else {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::audio, "audio export source is incomplete")};
    }
    if (result.data_size > std::numeric_limits<std::uint32_t>::max() - 36U) {
        return std::unexpected{
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::audio, "PCM is too large for RIFF/WAVE")};
    }
    const auto block = static_cast<std::uint16_t>(result.channels * result.width);
    const auto byte_rate = static_cast<std::uint64_t>(result.sample_rate) * block;
    if (byte_rate > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "waveform byte rate exceeds the WAVE header limit")};
    }
    return result;
}

void le16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

std::array<std::byte, wav_header_size> header(const WavLayout &layout) {
    std::array<std::byte, wav_header_size> result{};
    const auto tag = [&](std::size_t offset, std::string_view value) {
        std::ranges::transform(value, result.begin() + static_cast<std::ptrdiff_t>(offset),
                               [](char character) { return static_cast<std::byte>(character); });
    };
    tag(0U, "RIFF");
    le32(result, 4U, static_cast<std::uint32_t>(36U + layout.data_size));
    tag(8U, "WAVEfmt ");
    le32(result, 16U, 16U);
    le16(result, 20U, 1U);
    le16(result, 22U, layout.channels);
    le32(result, 24U, layout.sample_rate);
    const auto block = static_cast<std::uint16_t>(layout.channels * layout.width);
    le32(result, 28U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(layout.sample_rate) * block));
    le16(result, 32U, block);
    le16(result, 34U, static_cast<std::uint16_t>(layout.width * 8U));
    tag(36U, "data");
    le32(result, 40U, static_cast<std::uint32_t>(layout.data_size));
    return result;
}

std::byte data_byte(const WavSource &source, const WavLayout &layout, std::size_t offset) {
    if (source.physical != nullptr)
        return source.physical->pcm[offset];
    const auto frame_size = static_cast<std::size_t>(2U * layout.width);
    const auto frame = offset / frame_size;
    const auto within_frame = offset % frame_size;
    const auto member_index = within_frame / layout.width;
    const auto member_offset = within_frame % layout.width;
    const auto *member = member_index == 0U ? source.left : source.right;
    if (frame >= member->frame_count)
        return std::byte{};
    return member->pcm[frame * layout.width + member_offset];
}

void render_stereo_chunk(const WavSource &source, const WavLayout &layout, std::size_t byte_offset,
                         std::span<std::byte> output) {
    const auto frame_size = static_cast<std::size_t>(2U * layout.width);
    const auto first_frame = byte_offset / frame_size;
    const auto frame_count = output.size() / frame_size;
    if (layout.width == 1U) {
        for (std::size_t index = 0U; index < frame_count; ++index) {
            const auto frame = first_frame + index;
            output[index * 2U] = frame < source.left->frame_count ? source.left->pcm[frame] : std::byte{};
            output[index * 2U + 1U] = frame < source.right->frame_count ? source.right->pcm[frame] : std::byte{};
        }
        return;
    }
    for (std::size_t index = 0U; index < frame_count; ++index) {
        const auto frame = first_frame + index;
        const auto output_offset = index * 4U;
        if (frame < source.left->frame_count) {
            const auto input_offset = frame * 2U;
            output[output_offset] = source.left->pcm[input_offset];
            output[output_offset + 1U] = source.left->pcm[input_offset + 1U];
        } else {
            output[output_offset] = std::byte{};
            output[output_offset + 1U] = std::byte{};
        }
        if (frame < source.right->frame_count) {
            const auto input_offset = frame * 2U;
            output[output_offset + 2U] = source.right->pcm[input_offset];
            output[output_offset + 3U] = source.right->pcm[input_offset + 1U];
        } else {
            output[output_offset + 2U] = std::byte{};
            output[output_offset + 3U] = std::byte{};
        }
    }
}

} // namespace

WavSource WavSource::from_physical(const Waveform &waveform) noexcept { return {.physical = &waveform}; }

WavSource WavSource::from_stereo(const Waveform &left, const Waveform &right) noexcept {
    return {.left = &left, .right = &right};
}

Result<void> stream_wav(const WavSource &source, const WavChunkConsumer &consume,
                        const CancellationToken &cancellation) {
    const auto source_layout = layout(source);
    if (!source_layout)
        return std::unexpected{source_layout.error()};
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    const auto source_header = header(*source_layout);
    if (const auto consumed = consume(source_header); !consumed)
        return std::unexpected{consumed.error()};
    if (source.physical != nullptr) {
        for (std::size_t offset = 0U; offset < source.physical->pcm.size(); offset += stream_buffer_size) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto count = std::min(stream_buffer_size, source.physical->pcm.size() - offset);
            if (const auto consumed = consume(std::span{source.physical->pcm}.subspan(offset, count)); !consumed)
                return std::unexpected{consumed.error()};
        }
        return {};
    }
    std::array<std::byte, stream_buffer_size> buffer{};
    for (std::size_t offset = 0U; offset < source_layout->data_size; offset += buffer.size()) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto count = std::min(buffer.size(), source_layout->data_size - offset);
        render_stereo_chunk(source, *source_layout, offset, std::span{buffer}.first(count));
        if (const auto consumed = consume(std::span{buffer}.first(count)); !consumed)
            return std::unexpected{consumed.error()};
    }
    return {};
}

Result<bool> equal_wav(const WavSource &left, const WavSource &right, const CancellationToken &cancellation) {
    const auto left_layout = layout(left);
    if (!left_layout)
        return std::unexpected{left_layout.error()};
    const auto right_layout = layout(right);
    if (!right_layout)
        return std::unexpected{right_layout.error()};
    if (left_layout->channels != right_layout->channels || left_layout->width != right_layout->width ||
        left_layout->sample_rate != right_layout->sample_rate || left_layout->data_size != right_layout->data_size)
        return false;
    for (std::size_t offset = 0U; offset < left_layout->data_size; ++offset) {
        if ((offset % stream_buffer_size) == 0U) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
        }
        if (data_byte(left, *left_layout, offset) != data_byte(right, *right_layout, offset))
            return false;
    }
    return true;
}

Result<void> write_wav_atomic(const std::filesystem::path &path, const WavSource &source, bool overwrite,
                              const CancellationToken &cancellation) {
    if (!overwrite && std::filesystem::exists(path)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "refusing to replace an existing WAV")};
    }
    if (const auto source_layout = layout(source); !source_layout)
        return std::unexpected{source_layout.error()};
    std::error_code error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create WAV output directory")};
    }
    const auto temporary = text::temporary_sibling(path);
    if (!temporary)
        return std::unexpected{temporary.error()};
    {
        std::ofstream output{*temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            std::filesystem::remove(*temporary, error);
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open temporary WAV")};
        }
        const auto written = stream_wav(
            source,
            [&](std::span<const std::byte> bytes) -> Result<void> {
                output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!output) {
                    return std::unexpected{
                        make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary WAV")};
                }
                return {};
            },
            cancellation);
        if (!written) {
            output.close();
            std::filesystem::remove(*temporary, error);
            return std::unexpected{written.error()};
        }
    }
    if (overwrite)
        std::filesystem::remove(path, error);
    std::filesystem::rename(*temporary, path, error);
    if (error) {
        std::filesystem::remove(*temporary, error);
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not publish WAV atomically")};
    }
    return {};
}

} // namespace axk::audio_internal
