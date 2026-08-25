#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "axklib/audio.hpp"
#include "axklib/error.hpp"
#include "axklib/publication.hpp"

namespace axk::audio_internal {

struct WavSampleLoop {
    std::uint32_t identifier{};
    std::uint32_t type{};
    std::uint32_t start{};
    std::uint32_t inclusive_end{};
    std::uint32_t fraction{};
    std::uint32_t play_count{};

    friend bool operator==(const WavSampleLoop &, const WavSampleLoop &) = default;
};

struct WavSmplChunk {
    std::uint32_t manufacturer{};
    std::uint32_t product{};
    std::uint32_t sample_period_nanoseconds{};
    std::uint32_t midi_unity_note{};
    std::uint32_t midi_pitch_fraction{};
    std::uint32_t smpte_format{};
    std::uint32_t smpte_offset{};
    std::vector<WavSampleLoop> loops;
    std::vector<std::byte> sampler_specific_data;

    friend bool operator==(const WavSmplChunk &, const WavSmplChunk &) = default;
};

struct WavInstChunk {
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
    std::int8_t gain_decibels{};
    std::uint8_t key_low{};
    std::uint8_t key_high{};
    std::uint8_t velocity_low{};
    std::uint8_t velocity_high{};

    friend bool operator==(const WavInstChunk &, const WavInstChunk &) = default;
};

struct WavSamplerChunks {
    std::optional<WavSmplChunk> smpl;
    std::optional<WavInstChunk> inst;

    friend bool operator==(const WavSamplerChunks &, const WavSamplerChunks &) = default;
};

struct WavSource {
    const Waveform *physical{};
    const Waveform *left{};
    const Waveform *right{};
    WavSamplerChunks sampler;
    std::vector<PublicationWarning> warnings;

    [[nodiscard]] static WavSource from_physical(const Waveform &waveform);
    [[nodiscard]] static WavSource from_stereo(const Waveform &left, const Waveform &right);
};

using WavChunkConsumer = std::function<Result<void>(std::span<const std::byte>)>;

Result<void> stream_wav(const WavSource &source, const WavChunkConsumer &consume,
                        const CancellationToken &cancellation = {});
Result<bool> equal_wav(const WavSource &left, const WavSource &right, const CancellationToken &cancellation = {});
Result<PublicationOutcome> write_wav_atomic(const std::filesystem::path &path, const WavSource &source,
                                            bool overwrite = false, const CancellationToken &cancellation = {});

} // namespace axk::audio_internal
