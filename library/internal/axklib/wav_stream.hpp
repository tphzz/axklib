#pragma once

#include <filesystem>
#include <functional>
#include <span>

#include "axklib/audio.hpp"
#include "axklib/error.hpp"

namespace axk::audio_internal {

struct WavSource {
    const Waveform *physical{};
    const Waveform *left{};
    const Waveform *right{};

    [[nodiscard]] static WavSource from_physical(const Waveform &waveform) noexcept;
    [[nodiscard]] static WavSource from_stereo(const Waveform &left, const Waveform &right) noexcept;
};

using WavChunkConsumer = std::function<Result<void>(std::span<const std::byte>)>;

Result<void> stream_wav(const WavSource &source, const WavChunkConsumer &consume,
                        const CancellationToken &cancellation = {});
Result<bool> equal_wav(const WavSource &left, const WavSource &right, const CancellationToken &cancellation = {});
Result<void> write_wav_atomic(const std::filesystem::path &path, const WavSource &source, bool overwrite = false,
                              const CancellationToken &cancellation = {});

} // namespace axk::audio_internal
