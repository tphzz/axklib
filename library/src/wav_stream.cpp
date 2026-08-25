#include "axklib/wav_stream.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

#include "axklib/file_publication.hpp"
#include "axklib/utf8.hpp"
#include "axklib/wav_sampler_mapping.hpp"

namespace axk::audio_internal {
namespace {

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
    if (result.data_size > std::numeric_limits<std::uint32_t>::max()) {
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

void append_tag(std::vector<std::byte> &bytes, std::string_view value) {
    std::ranges::transform(value, std::back_inserter(bytes),
                           [](char character) { return static_cast<std::byte>(character); });
}

void append_u32(std::vector<std::byte> &bytes, std::uint32_t value) {
    const auto offset = bytes.size();
    bytes.resize(offset + 4U);
    le32(bytes, offset, value);
}

void append_chunk(std::vector<std::byte> &bytes, std::string_view id, std::span<const std::byte> payload) {
    append_tag(bytes, id);
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (payload.size() % 2U != 0U)
        bytes.push_back(std::byte{});
}

Result<std::vector<std::byte>> smpl_payload(const WavSmplChunk &smpl) {
    constexpr std::size_t header_size = 36U;
    constexpr std::size_t loop_size = 24U;
    if (smpl.loops.size() > std::numeric_limits<std::uint32_t>::max() ||
        smpl.loops.size() > (std::numeric_limits<std::size_t>::max() - header_size) / loop_size) {
        return std::unexpected{
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::audio, "WAV smpl loop table is too large")};
    }
    const auto loop_bytes = smpl.loops.size() * loop_size;
    constexpr auto maximum_chunk_size = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (loop_bytes > maximum_chunk_size - header_size ||
        smpl.sampler_specific_data.size() > maximum_chunk_size - header_size - loop_bytes) {
        return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::audio,
                                          "WAV smpl sampler-specific data is too large")};
    }
    std::vector<std::byte> result(header_size + loop_bytes + smpl.sampler_specific_data.size());
    le32(result, 0U, smpl.manufacturer);
    le32(result, 4U, smpl.product);
    le32(result, 8U, smpl.sample_period_nanoseconds);
    le32(result, 12U, smpl.midi_unity_note);
    le32(result, 16U, smpl.midi_pitch_fraction);
    le32(result, 20U, smpl.smpte_format);
    le32(result, 24U, smpl.smpte_offset);
    le32(result, 28U, static_cast<std::uint32_t>(smpl.loops.size()));
    le32(result, 32U, static_cast<std::uint32_t>(smpl.sampler_specific_data.size()));
    for (std::size_t index = 0U; index < smpl.loops.size(); ++index) {
        const auto offset = header_size + index * loop_size;
        const auto &loop = smpl.loops[index];
        le32(result, offset, loop.identifier);
        le32(result, offset + 4U, loop.type);
        le32(result, offset + 8U, loop.start);
        le32(result, offset + 12U, loop.inclusive_end);
        le32(result, offset + 16U, loop.fraction);
        le32(result, offset + 20U, loop.play_count);
    }
    std::ranges::copy(smpl.sampler_specific_data,
                      result.begin() + static_cast<std::ptrdiff_t>(header_size + loop_bytes));
    return result;
}

std::array<std::byte, 7> inst_payload(const WavInstChunk &inst) {
    return {static_cast<std::byte>(inst.root_key),
            static_cast<std::byte>(static_cast<std::uint8_t>(inst.fine_tune_cents)),
            static_cast<std::byte>(static_cast<std::uint8_t>(inst.gain_decibels)),
            static_cast<std::byte>(inst.key_low),
            static_cast<std::byte>(inst.key_high),
            static_cast<std::byte>(inst.velocity_low),
            static_cast<std::byte>(inst.velocity_high)};
}

Result<std::vector<std::byte>> prefix(const WavLayout &layout, const WavSamplerChunks &sampler) {
    std::vector<std::byte> result;
    append_tag(result, "RIFF");
    append_u32(result, 0U);
    append_tag(result, "WAVE");
    std::array<std::byte, 16> format{};
    le16(format, 0U, 1U);
    le16(format, 2U, layout.channels);
    le32(format, 4U, layout.sample_rate);
    const auto block = static_cast<std::uint16_t>(layout.channels * layout.width);
    le32(format, 8U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(layout.sample_rate) * block));
    le16(format, 12U, block);
    le16(format, 14U, static_cast<std::uint16_t>(layout.width * 8U));
    append_chunk(result, "fmt ", format);
    if (sampler.smpl) {
        auto payload = smpl_payload(*sampler.smpl);
        if (!payload)
            return std::unexpected{payload.error()};
        append_chunk(result, "smpl", *payload);
    }
    if (sampler.inst) {
        const auto payload = inst_payload(*sampler.inst);
        append_chunk(result, "inst", payload);
    }
    append_tag(result, "data");
    append_u32(result, static_cast<std::uint32_t>(layout.data_size));
    const auto total_size = result.size() + layout.data_size + (layout.data_size % 2U);
    if (total_size < 8U || total_size - 8U > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::audio, "WAV exceeds the RIFF size limit")};
    }
    le32(result, 4U, static_cast<std::uint32_t>(total_size - 8U));
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

WavSource WavSource::from_physical(const Waveform &waveform) {
    WavSource result{.physical = &waveform, .left = nullptr, .right = nullptr, .sampler = {}, .warnings = {}};
    apply_sampler_mapping(result, map_a_series_sampler_metadata(waveform_sampler_parameters(waveform)));
    return result;
}

WavSource WavSource::from_stereo(const Waveform &left, const Waveform &right) {
    WavSource result{.physical = nullptr, .left = &left, .right = &right, .sampler = {}, .warnings = {}};
    const auto metadata_matches = left.format.sample_rate == right.format.sample_rate &&
                                  left.root_key == right.root_key && left.fine_tune_cents == right.fine_tune_cents &&
                                  left.loop_mode == right.loop_mode && left.loop_start == right.loop_start &&
                                  left.loop_length == right.loop_length;
    if (metadata_matches) {
        auto parameters = waveform_sampler_parameters(left);
        parameters.frame_count = std::max(left.frame_count, right.frame_count);
        parameters.context = "stereo Wave Data " + left.name + " / " + right.name;
        apply_sampler_mapping(result, map_a_series_sampler_metadata(parameters));
    } else {
        result.warnings.push_back(
            {"wav_stereo_sampler_metadata_omitted",
             "Stereo Wave Data members disagree on pitch or loop metadata; WAV smpl and inst chunks were omitted"});
    }
    return result;
}

Result<void> stream_wav(const WavSource &source, const WavChunkConsumer &consume,
                        const CancellationToken &cancellation) {
    const auto source_layout = layout(source);
    if (!source_layout)
        return std::unexpected{source_layout.error()};
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    const auto source_prefix = prefix(*source_layout, source.sampler);
    if (!source_prefix)
        return std::unexpected{source_prefix.error()};
    if (const auto consumed = consume(*source_prefix); !consumed)
        return std::unexpected{consumed.error()};
    if (source.physical != nullptr) {
        for (std::size_t offset = 0U; offset < source.physical->pcm.size(); offset += stream_buffer_size) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto count = std::min(stream_buffer_size, source.physical->pcm.size() - offset);
            if (const auto consumed = consume(std::span{source.physical->pcm}.subspan(offset, count)); !consumed)
                return std::unexpected{consumed.error()};
        }
        if (source_layout->data_size % 2U != 0U) {
            constexpr std::array padding{std::byte{}};
            if (const auto consumed = consume(padding); !consumed)
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
    if (source_layout->data_size % 2U != 0U) {
        constexpr std::array padding{std::byte{}};
        if (const auto consumed = consume(padding); !consumed)
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
    if (left.sampler != right.sampler || left_layout->channels != right_layout->channels ||
        left_layout->width != right_layout->width || left_layout->sample_rate != right_layout->sample_rate ||
        left_layout->data_size != right_layout->data_size)
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

Result<PublicationOutcome> write_wav_atomic(const std::filesystem::path &path, const WavSource &source, bool overwrite,
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
    auto temporary = detail::TemporaryPublication::create(
        path, [&](const detail::TemporaryFileSink &sink) { return stream_wav(source, sink, cancellation); });
    if (!temporary)
        return std::unexpected{temporary.error()};
    const auto mode = overwrite ? detail::PublicationMode::replace_existing : detail::PublicationMode::create_only;
    auto published = temporary->publish(mode);
    if (!published)
        return std::unexpected{published.error()};
    auto outcome = std::move(*published);
    outcome.warnings.insert(outcome.warnings.end(), source.warnings.begin(), source.warnings.end());
    return outcome;
}

} // namespace axk::audio_internal
