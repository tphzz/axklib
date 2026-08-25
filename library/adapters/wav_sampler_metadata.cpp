#include "audio_import_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axk::audio_import_detail {
namespace {

enum class ByteOrder : std::uint8_t { little, big };

struct Chunk {
    std::array<char, 4> id{};
    std::uint64_t payload_offset{};
    std::uint64_t payload_size{};
};

struct SmplPitch {
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
};

struct InstMetadata {
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
    std::int8_t gain_decibels{};
    std::uint8_t key_low{};
    std::uint8_t key_high{};
    std::uint8_t velocity_low{};
    std::uint8_t velocity_high{};
};

bool matches(const std::array<std::byte, 4> &value, std::string_view expected) {
    return expected.size() == value.size() &&
           std::equal(value.begin(), value.end(), expected.begin(),
                      [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
}

std::uint32_t u32(std::span<const std::byte> bytes, ByteOrder order) {
    std::uint32_t result{};
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto shift = order == ByteOrder::big ? (3U - index) * 8U : index * 8U;
        result |= std::to_integer<std::uint32_t>(bytes[index]) << shift;
    }
    return result;
}

Result<std::array<std::byte, 4>> read_four(const RandomAccessReader &reader, std::uint64_t offset) {
    std::array<std::byte, 4> bytes{};
    if (auto read = reader.read_exact_at(offset, bytes); !read)
        return std::unexpected{read.error()};
    return bytes;
}

Result<std::uint32_t> read_u32(const RandomAccessReader &reader, std::uint64_t offset, ByteOrder order) {
    auto bytes = read_four(reader, offset);
    if (!bytes)
        return std::unexpected{bytes.error()};
    return u32(*bytes, order);
}

Result<std::vector<Chunk>> chunks(const RandomAccessReader &reader, ByteOrder order, bool rf64) {
    std::vector<Chunk> result;
    const auto file_size = reader.size();
    if (file_size < 12U)
        return result;
    std::uint64_t rf64_data_size{};
    std::uint64_t offset = 12U;
    while (offset <= file_size && file_size - offset >= 8U) {
        auto id = read_four(reader, offset);
        auto size = read_u32(reader, offset + 4U, order);
        if (!id)
            return std::unexpected{id.error()};
        if (!size)
            return std::unexpected{size.error()};
        const std::array<char, 4> chunk_id{
            static_cast<char>(std::to_integer<unsigned char>((*id)[0])),
            static_cast<char>(std::to_integer<unsigned char>((*id)[1])),
            static_cast<char>(std::to_integer<unsigned char>((*id)[2])),
            static_cast<char>(std::to_integer<unsigned char>((*id)[3])),
        };
        std::uint64_t payload_size = *size;
        if (rf64 && chunk_id == std::array{'d', 's', '6', '4'} && payload_size >= 16U &&
            payload_size <= file_size - (offset + 8U)) {
            std::array<std::byte, 8> data_size{};
            if (auto read = reader.read_exact_at(offset + 16U, data_size); !read)
                return std::unexpected{read.error()};
            for (std::size_t index = 0; index < data_size.size(); ++index)
                rf64_data_size |= std::to_integer<std::uint64_t>(data_size[index]) << (index * 8U);
        }
        if (rf64 && chunk_id == std::array{'d', 'a', 't', 'a'} && *size == 0xffffffffU)
            payload_size = rf64_data_size;
        if (payload_size > file_size - (offset + 8U))
            break;
        result.push_back({chunk_id, offset + 8U, payload_size});
        const auto padded = payload_size + (payload_size % 2U);
        if (padded > file_size - (offset + 8U))
            break;
        offset += 8U + padded;
    }
    return result;
}

std::optional<SmplPitch> smpl_pitch(std::uint32_t unity_note, std::uint32_t fraction) {
    if (unity_note > 127U)
        return std::nullopt;
    const auto cents = static_cast<int>(std::lround(static_cast<double>(fraction) * 100.0 / 4'294'967'296.0));
    auto root_key = static_cast<int>(unity_note);
    auto fine_tune = cents;
    if (fine_tune > 63) {
        ++root_key;
        fine_tune -= 100;
    }
    if (root_key > 127 || fine_tune < -63 || fine_tune > 63)
        return std::nullopt;
    return SmplPitch{static_cast<std::uint8_t>(root_key), static_cast<std::int8_t>(fine_tune)};
}

std::uint64_t scale_boundary(std::uint64_t frame, std::uint32_t source_rate, std::uint32_t output_rate) {
    const auto whole = frame / source_rate;
    const auto remainder = frame % source_rate;
    return whole * output_rate + (remainder * output_rate + source_rate / 2U) / source_rate;
}

void warning(WavSamplerMetadata &metadata, std::string code, std::string message) {
    metadata.issues.push_back({std::move(code), std::move(message), false});
}

Result<std::optional<InstMetadata>> parse_inst(const RandomAccessReader &reader, const Chunk &chunk,
                                               WavSamplerMetadata &metadata) {
    if (chunk.payload_size < 7U) {
        warning(metadata, "wav_inst_malformed", "WAV inst metadata is truncated and was ignored");
        return std::optional<InstMetadata>{};
    }
    std::array<std::byte, 7> bytes{};
    if (auto read = reader.read_exact_at(chunk.payload_offset, bytes); !read)
        return std::unexpected{read.error()};
    InstMetadata result{
        std::to_integer<std::uint8_t>(bytes[0]),
        static_cast<std::int8_t>(std::to_integer<std::uint8_t>(bytes[1])),
        static_cast<std::int8_t>(std::to_integer<std::uint8_t>(bytes[2])),
        std::to_integer<std::uint8_t>(bytes[3]),
        std::to_integer<std::uint8_t>(bytes[4]),
        std::to_integer<std::uint8_t>(bytes[5]),
        std::to_integer<std::uint8_t>(bytes[6]),
    };
    if (result.root_key > 127U || result.fine_tune_cents < -50 || result.fine_tune_cents > 50) {
        warning(metadata, "wav_inst_pitch_invalid", "WAV inst pitch is outside A-series limits and was ignored");
        result.root_key = 255U;
    }
    if (result.key_low > 127U || result.key_high > 127U || result.key_low > result.key_high ||
        result.velocity_low > 127U || result.velocity_high > 127U || result.velocity_low > result.velocity_high) {
        warning(metadata, "wav_inst_range_invalid", "WAV inst key or velocity ranges are invalid and were ignored");
        result.key_low = 255U;
    }
    if (result.gain_decibels != 0)
        warning(metadata, "wav_inst_gain_unsupported", "WAV inst gain is not mapped to an A-series Sample");
    return std::optional<InstMetadata>{result};
}

Result<std::optional<SmplPitch>> parse_smpl(const RandomAccessReader &reader, const Chunk &chunk, ByteOrder order,
                                            std::uint64_t source_frames, std::uint32_t source_rate,
                                            std::uint32_t output_rate, WavSamplerMetadata &metadata) {
    if (chunk.payload_size < 36U) {
        warning(metadata, "wav_smpl_malformed", "WAV smpl metadata is truncated and was ignored");
        return std::optional<SmplPitch>{};
    }
    std::array<std::byte, 36> header{};
    if (auto read = reader.read_exact_at(chunk.payload_offset, header); !read)
        return std::unexpected{read.error()};
    const auto pitch =
        smpl_pitch(u32(std::span{header}.subspan<12U, 4U>(), order), u32(std::span{header}.subspan<16U, 4U>(), order));
    if (!pitch)
        warning(metadata, "wav_smpl_pitch_invalid", "WAV smpl pitch is outside A-series limits and was ignored");

    const auto loop_count = u32(std::span{header}.subspan<28U, 4U>(), order);
    const auto sampler_data_size = u32(std::span{header}.subspan<32U, 4U>(), order);
    const auto loops_size = static_cast<std::uint64_t>(loop_count) * 24U;
    if (loops_size > chunk.payload_size - 36U || sampler_data_size > chunk.payload_size - 36U - loops_size) {
        warning(metadata, "wav_smpl_malformed", "WAV smpl loop or sampler-specific data is truncated and was ignored");
        return pitch;
    }
    if (sampler_data_size != 0U) {
        warning(metadata, "wav_sampler_specific_data_ignored",
                "WAV smpl contains sampler-specific data that is not mapped to an A-series Sample");
    }
    if (loop_count == 0U)
        return pitch;
    if (loop_count != 1U) {
        warning(metadata, "wav_sampler_multiple_loops_unsupported",
                "WAV contains multiple sampler loops; imported Sample will use one-shot playback");
        return pitch;
    }
    std::array<std::byte, 24> loop_bytes{};
    if (auto read = reader.read_exact_at(chunk.payload_offset + 36U, loop_bytes); !read)
        return std::unexpected{read.error()};
    const auto loop = std::span{loop_bytes};
    const auto type = u32(loop.subspan<4U, 4U>(), order);
    const auto start = u32(loop.subspan<8U, 4U>(), order);
    const auto inclusive_end = u32(loop.subspan<12U, 4U>(), order);
    const auto fraction = u32(loop.subspan<16U, 4U>(), order);
    const auto play_count = u32(loop.subspan<20U, 4U>(), order);
    if (type != 0U) {
        const auto [code, description] = type == 1U
                                             ? std::pair{"wav_sampler_alternating_loop_unsupported", "alternating"}
                                         : type == 2U ? std::pair{"wav_sampler_backward_loop_unsupported", "backward"}
                                                      : std::pair{"wav_sampler_loop_type_unsupported", "nonstandard"};
        warning(metadata, code,
                std::string{"WAV contains a "} + description +
                    " sampler loop; imported Sample will use one-shot playback");
        return pitch;
    }
    if (fraction != 0U) {
        warning(metadata, "wav_sampler_loop_fraction_unsupported",
                "WAV sampler loop uses a fractional boundary; imported Sample will use one-shot playback");
        return pitch;
    }
    if (start > inclusive_end || inclusive_end >= source_frames) {
        warning(metadata, "wav_sampler_loop_range_invalid",
                "WAV sampler loop is out of range; imported Sample will use one-shot playback");
        return pitch;
    }
    if (play_count != 0U) {
        warning(metadata, "wav_sampler_loop_repeat_count_normalized",
                "WAV sampler loop has a finite repeat count; imported Sample will use an indefinite forward loop");
    }
    const auto output_start = scale_boundary(start, source_rate, output_rate);
    const auto output_end = scale_boundary(static_cast<std::uint64_t>(inclusive_end) + 1U, source_rate, output_rate);
    if (output_end <= output_start) {
        warning(metadata, "wav_sampler_loop_unsupported",
                "WAV sampler loop becomes empty at the target rate; imported Sample will use one-shot playback");
        return pitch;
    }
    metadata.settings.loop_mode = AudioSamplerLoopMode::forward_loop;
    metadata.settings.loop_start_frame = output_start;
    metadata.settings.loop_length_frames = output_end - output_start;
    metadata.settings.loop_source = "WAV_SMPL";
    return pitch;
}

} // namespace

Result<WavSamplerMetadata> inspect_wav_sampler_metadata(const RandomAccessReader &reader, std::uint64_t source_frames,
                                                        std::uint32_t source_rate, std::uint32_t output_rate) {
    WavSamplerMetadata metadata;
    if (reader.size() < 12U || source_frames == 0U || source_rate == 0U || output_rate == 0U)
        return metadata;
    auto container = read_four(reader, 0U);
    auto wave = read_four(reader, 8U);
    if (!container)
        return std::unexpected{container.error()};
    if (!wave)
        return std::unexpected{wave.error()};
    const bool riff = matches(*container, "RIFF");
    const bool rifx = matches(*container, "RIFX");
    const bool rf64 = matches(*container, "RF64");
    if ((!riff && !rifx && !rf64) || !matches(*wave, "WAVE"))
        return metadata;
    const auto order = rifx ? ByteOrder::big : ByteOrder::little;
    auto found = chunks(reader, order, rf64);
    if (!found)
        return std::unexpected{found.error()};
    std::optional<Chunk> smpl;
    std::optional<Chunk> inst;
    for (const auto &chunk : *found) {
        if (!smpl && chunk.id == std::array{'s', 'm', 'p', 'l'})
            smpl = chunk;
        else if (!inst && chunk.id == std::array{'i', 'n', 's', 't'})
            inst = chunk;
    }
    std::optional<InstMetadata> instrument;
    if (inst) {
        auto parsed = parse_inst(reader, *inst, metadata);
        if (!parsed)
            return std::unexpected{parsed.error()};
        instrument = *parsed;
        if (instrument && instrument->key_low != 255U) {
            metadata.settings.key_low = instrument->key_low;
            metadata.settings.key_high = instrument->key_high;
            metadata.settings.velocity_low = instrument->velocity_low;
            metadata.settings.velocity_high = instrument->velocity_high;
            metadata.settings.range_source = "WAV_INST";
        }
    }
    std::optional<SmplPitch> pitch;
    if (smpl) {
        auto parsed = parse_smpl(reader, *smpl, order, source_frames, source_rate, output_rate, metadata);
        if (!parsed)
            return std::unexpected{parsed.error()};
        pitch = *parsed;
    }
    if (pitch) {
        metadata.settings.root_key = pitch->root_key;
        metadata.settings.fine_tune_cents = pitch->fine_tune_cents;
        metadata.settings.pitch_source = "WAV_SMPL";
        if (instrument && instrument->root_key != 255U &&
            (instrument->root_key != pitch->root_key || instrument->fine_tune_cents != pitch->fine_tune_cents)) {
            warning(metadata, "wav_sampler_pitch_conflict",
                    "WAV smpl and inst pitch metadata disagree; smpl pitch takes precedence");
        }
    } else if (instrument && instrument->root_key != 255U) {
        metadata.settings.root_key = instrument->root_key;
        metadata.settings.fine_tune_cents = instrument->fine_tune_cents;
        metadata.settings.pitch_source = "WAV_INST";
    }
    return metadata;
}

} // namespace axk::audio_import_detail
