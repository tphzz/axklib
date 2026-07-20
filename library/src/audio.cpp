#include "axklib/audio.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "axklib/bytes.hpp"
#include "axklib/media.hpp"
#include "axklib/wav_stream.hpp"

namespace axk {
namespace {

constexpr std::size_t maximum_preview_bins = 65'536;

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

void le16(std::vector<std::byte> &bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void le32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

std::int32_t sample_value(const Waveform &waveform, std::uint64_t frame, std::uint16_t channel) {
    const auto width = waveform.format.sample_width_bytes;
    const auto index = static_cast<std::size_t>((frame * waveform.format.channels + channel) * width);
    if (width == 1U)
        return std::to_integer<std::uint8_t>(waveform.pcm[index]) - 128;
    const auto low = std::to_integer<std::uint8_t>(waveform.pcm[index]);
    const auto high = std::to_integer<std::uint8_t>(waveform.pcm[index + 1U]);
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(low | (high << 8U)));
}

} // namespace

static Result<Waveform> decode_waveform_payload(const CurrentSmpl &decoded, std::string object_key,
                                                std::filesystem::path source_path, PartitionIndex partition,
                                                SfsId sfs_id, std::string name, std::span<const std::byte> payload) {
    const auto start = decoded.stored_pcm_offset;
    const auto size = decoded.stored_pcm_bytes;
    if (start > payload.size() || size > payload.size() - start) {
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::audio, "SMPL PCM span lies outside its object")};
    }
    const auto stored = payload.subspan(start, size);
    Waveform result;
    result.object_key = std::move(object_key);
    result.source_path = std::move(source_path);
    result.partition = partition;
    result.sfs_id = sfs_id;
    result.name = std::move(name);
    result.format.sample_rate = decoded.sample_rate.value;
    result.stored_sample_width_bytes = decoded.stored_sample_width_bytes.value;
    result.stored_payload_size = size;
    result.root_key = decoded.root_key.value;
    result.fine_tune_cents = decoded.fine_tune_cents.value;
    result.loop_mode = decoded.loop_mode.value;
    result.loop_mode_label = decoded.loop_mode_label;
    result.loop_start = decoded.loop_start_frame.value;
    result.loop_length = decoded.loop_length_frames.value;
    if (result.stored_sample_width_bytes == 2U) {
        bool alternating = stored.size() >= 2U;
        const auto limit = stored.size() - (stored.size() % 2U);
        for (std::size_t offset = 1; offset < limit; offset += 2U) {
            const auto expected = offset % 4U == 1U ? 0x55U : 0xaaU;
            alternating &= std::to_integer<std::uint8_t>(stored[offset]) == expected;
        }
        if (alternating) {
            result.format.sample_width_bytes = 1;
            result.stored_payload_transform = "alternating-byte-signed-high-byte";
            result.alternating_byte_payload_detected = true;
            result.pcm.reserve((stored.size() + 1U) / 2U);
            for (std::size_t offset = 0; offset < stored.size(); offset += 2U) {
                result.pcm.push_back(
                    static_cast<std::byte>((std::to_integer<std::uint8_t>(stored[offset]) + 128U) & 0xffU));
            }
        } else {
            if (stored.size() % 2U != 0U) {
                return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::audio,
                                                  "16-bit SMPL PCM has an odd byte count")};
            }
            result.format.sample_width_bytes = 2;
            result.stored_payload_transform = "byteswap16";
            result.pcm.resize(stored.size());
            for (std::size_t offset = 0; offset < stored.size(); offset += 2U) {
                result.pcm[offset] = stored[offset + 1U];
                result.pcm[offset + 1U] = stored[offset];
            }
        }
    } else if (result.stored_sample_width_bytes == 1U) {
        result.format.sample_width_bytes = 1;
        result.stored_payload_transform = "raw";
        result.pcm.assign(stored.begin(), stored.end());
    } else {
        return std::unexpected{make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "Wave Data (SMPL) sample width is unsupported")};
    }
    result.frame_count = result.pcm.size() / result.format.sample_width_bytes;
    return result;
}

Result<Waveform> decode_waveform(const Container &container, const ObjectSnapshot &snapshot,
                                 const CancellationToken &cancellation) {
    const auto *decoded = std::get_if<CurrentSmpl>(&snapshot.object.payload);
    if (decoded == nullptr) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::audio, "waveform decode requires a SMPL object")};
    }
    auto payload = container.read_record_data(snapshot.partition, snapshot.sfs_id,
                                              std::numeric_limits<std::size_t>::max(), cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    return decode_waveform_payload(*decoded, snapshot.key, container.source_path(), snapshot.partition, snapshot.sfs_id,
                                   snapshot.object.header.name, *payload);
}

Result<Waveform> decode_waveform(const ObjectSnapshot &snapshot, const std::filesystem::path &source_path) {
    const auto *decoded = std::get_if<CurrentSmpl>(&snapshot.object.payload);
    if (decoded == nullptr) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::audio, "waveform decode requires a SMPL object")};
    }
    return decode_waveform_payload(*decoded, snapshot.key, source_path, snapshot.partition, snapshot.sfs_id,
                                   snapshot.object.header.name, snapshot.raw_payload);
}

Result<Waveform> decode_waveform(const MediaObject &object) {
    const auto *decoded = std::get_if<CurrentSmpl>(&object.decoded.payload);
    if (decoded == nullptr) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::audio, "waveform decode requires a SMPL object")};
    }
    return decode_waveform_payload(*decoded, object.key, object.logical_path, PartitionIndex{0}, SfsId{0},
                                   object.decoded.header.name, object.raw_payload);
}

Result<std::vector<std::byte>> wav_bytes(const Waveform &waveform) {
    if (const auto valid = validate_waveform(waveform); !valid) {
        return std::unexpected{valid.error()};
    }
    const auto data_size = waveform.pcm.size();
    if (data_size > std::numeric_limits<std::uint32_t>::max() - 36U) {
        return std::unexpected{
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::audio, "PCM is too large for RIFF/WAVE")};
    }
    std::vector<std::byte> result(44U + data_size);
    const auto write_tag = [&](std::size_t offset, std::string_view tag) {
        std::ranges::transform(tag, result.begin() + static_cast<std::ptrdiff_t>(offset),
                               [](char value) { return static_cast<std::byte>(value); });
    };
    write_tag(0, "RIFF");
    le32(result, 4, static_cast<std::uint32_t>(36U + data_size));
    write_tag(8, "WAVEfmt ");
    le32(result, 16, 16);
    le16(result, 20, 1);
    le16(result, 22, waveform.format.channels);
    le32(result, 24, waveform.format.sample_rate);
    const auto block = static_cast<std::uint16_t>(waveform.format.channels * waveform.format.sample_width_bytes);
    const auto byte_rate = static_cast<std::uint64_t>(waveform.format.sample_rate) * block;
    if (byte_rate > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                                          "waveform byte rate exceeds the WAVE header limit")};
    }
    le32(result, 28, static_cast<std::uint32_t>(byte_rate));
    le16(result, 32, block);
    le16(result, 34, static_cast<std::uint16_t>(waveform.format.sample_width_bytes * 8U));
    write_tag(36, "data");
    le32(result, 40, static_cast<std::uint32_t>(data_size));
    std::ranges::copy(waveform.pcm, result.begin() + 44);
    return result;
}

Result<void> write_wav_atomic(const std::filesystem::path &path, const Waveform &waveform, bool overwrite) {
    return audio_internal::write_wav_atomic(path, audio_internal::WavSource::from_physical(waveform), overwrite);
}

Result<PreviewEnvelope> build_preview_envelope(const Waveform &waveform, std::size_t bin_count) {
    if (const auto valid = validate_waveform(waveform); !valid) {
        return std::unexpected{valid.error()};
    }
    if (bin_count == 0U || bin_count > maximum_preview_bins) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::audio, "preview bin count is outside bounds")};
    }
    PreviewEnvelope result;
    result.frame_count = waveform.frame_count;
    const auto used_bins = static_cast<std::size_t>(std::min<std::uint64_t>(bin_count, waveform.frame_count));
    result.bins.reserve(used_bins);
    for (std::size_t bin = 0; bin < used_bins; ++bin) {
        const auto start = waveform.frame_count * bin / used_bins;
        const auto end = waveform.frame_count * (bin + 1U) / used_bins;
        PreviewBin item{std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::min()};
        for (auto frame = start; frame < end; ++frame) {
            for (std::uint16_t channel = 0; channel < waveform.format.channels; ++channel) {
                const auto value = sample_value(waveform, frame, channel);
                item.minimum = std::min(item.minimum, value);
                item.maximum = std::max(item.maximum, value);
            }
        }
        result.bins.push_back(item);
    }
    return result;
}

StereoRenderDecision stereo_render_decision(const Waveform &left, const Waveform &right) {
    if (left.format.channels != 1U || right.format.channels != 1U) {
        return {false, "member-is-not-mono"};
    }
    if (left.format.sample_rate != right.format.sample_rate) {
        return {false, "sample-rate-mismatch"};
    }
    if (left.format.sample_width_bytes != right.format.sample_width_bytes) {
        return {false, "sample-width-mismatch"};
    }
    const auto frames = std::max(left.frame_count, right.frame_count);
    return {true, "confirmed-compatible-members", frames, frames - left.frame_count, frames - right.frame_count};
}

Result<Waveform> render_stereo(const Waveform &left, const Waveform &right) {
    if (const auto valid = validate_waveform(left); !valid) {
        return std::unexpected{valid.error()};
    }
    if (const auto valid = validate_waveform(right); !valid) {
        return std::unexpected{valid.error()};
    }
    const auto decision = stereo_render_decision(left, right);
    if (!decision.renderable) {
        return std::unexpected{
            make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio, decision.reason_code)};
    }
    Waveform result = left;
    result.object_key = left.object_key + "+" + right.object_key;
    result.name = left.name;
    result.format.channels = 2;
    result.frame_count = decision.output_frame_count;
    result.pcm.clear();
    const auto width = left.format.sample_width_bytes;
    if (result.frame_count > std::numeric_limits<std::size_t>::max() / (2U * width)) {
        return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::audio,
                                          "rendered stereo PCM exceeds the addressable size")};
    }
    result.pcm.reserve(static_cast<std::size_t>(result.frame_count) * 2U * width);
    const std::array<std::byte, 2> zero{};
    for (std::uint64_t frame = 0; frame < result.frame_count; ++frame) {
        for (const auto *member : std::array{&left, &right}) {
            if (frame < member->frame_count) {
                const auto start = static_cast<std::size_t>(frame * width);
                const auto begin = static_cast<std::ptrdiff_t>(start);
                const auto end = static_cast<std::ptrdiff_t>(start + width);
                result.pcm.insert(result.pcm.end(), member->pcm.begin() + begin, member->pcm.begin() + end);
            } else {
                result.pcm.insert(result.pcm.end(), zero.begin(), zero.begin() + width);
            }
        }
    }
    return result;
}

} // namespace axk
