#pragma once

#include <string_view>

#include "axklib/audio_export.hpp"
#include "axklib/wav_stream.hpp"

namespace axk::audio_export_detail {

[[nodiscard]] audio_internal::WavSource
sample_wav_source(const SampleExport &sample, const PhysicalWaveformExport &waveform, std::string_view role);
[[nodiscard]] audio_internal::WavSource stereo_sample_wav_source(const SampleExport &sample,
                                                                 const PhysicalWaveformExport &left,
                                                                 const PhysicalWaveformExport &right);

} // namespace axk::audio_export_detail
