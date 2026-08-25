#include "axklib/audio_export_wav_source.hpp"

#include <algorithm>
#include <string>

#include "axklib/wav_sampler_mapping.hpp"

namespace axk::audio_export_detail {
namespace {

const CurrentSbnkMember *member(const SampleExport &sample, std::string_view role) {
    if (role == "right")
        return sample.decoded.right ? &*sample.decoded.right : nullptr;
    return &sample.decoded.left;
}

audio_internal::ASeriesSamplerParameters parameters(const SampleExport &sample, const Waveform &waveform,
                                                    const CurrentSbnkMember &source, std::string context) {
    return {
        .sample_rate = waveform.format.sample_rate,
        .frame_count = waveform.frame_count,
        .root_key = source.root_key,
        .fine_tune_cents = source.fine_tune_cents,
        .key_low = sample.key_low == 255U ? source.root_key : sample.key_low,
        .key_high = sample.key_high == 128U ? source.root_key : sample.key_high,
        .velocity_low = sample.decoded.velocity_range_low,
        .velocity_high = sample.decoded.velocity_range_high,
        .loop_mode = sample.decoded.loop_mode,
        .loop_start = source.loop_start_frame,
        .loop_length = source.loop_length_frames,
        .context = std::move(context),
    };
}

void add_coarse_tune_warning(const SampleExport &sample, audio_internal::WavSource &source) {
    if (sample.coarse_tune == 0)
        return;
    source.warnings.push_back(
        {"wav_sample_coarse_tune_omitted", "Sample " + sample.display_name +
                                               " uses coarse tuning, which WAV smpl and inst metadata cannot "
                                               "represent; SFZ transpose remains authoritative"});
}

bool stereo_members_agree(const CurrentSbnkMember &left, const CurrentSbnkMember &right) {
    return left.root_key == right.root_key && left.fine_tune_cents == right.fine_tune_cents &&
           left.wave_start_frame == right.wave_start_frame && left.wave_length_frames == right.wave_length_frames &&
           left.loop_start_frame == right.loop_start_frame && left.loop_length_frames == right.loop_length_frames;
}

} // namespace

audio_internal::WavSource sample_wav_source(const SampleExport &sample, const PhysicalWaveformExport &waveform,
                                            std::string_view role) {
    auto result = audio_internal::WavSource::from_physical(waveform.waveform);
    result.sampler = {};
    result.warnings.clear();
    if (const auto *source = member(sample, role); source != nullptr) {
        audio_internal::apply_sampler_mapping(
            result, audio_internal::map_a_series_sampler_metadata(
                        parameters(sample, waveform.waveform, *source, "Sample " + sample.display_name)));
    } else {
        result.warnings.push_back({"wav_sample_sampler_metadata_omitted",
                                   "Sample " + sample.display_name +
                                       " has no matching member metadata; WAV smpl and inst chunks were omitted"});
    }
    add_coarse_tune_warning(sample, result);
    return result;
}

audio_internal::WavSource stereo_sample_wav_source(const SampleExport &sample, const PhysicalWaveformExport &left,
                                                   const PhysicalWaveformExport &right) {
    auto result = audio_internal::WavSource::from_stereo(left.waveform, right.waveform);
    result.sampler = {};
    result.warnings.clear();
    if (sample.decoded.right && stereo_members_agree(sample.decoded.left, *sample.decoded.right)) {
        auto mapped = parameters(sample, left.waveform, sample.decoded.left, "stereo Sample " + sample.display_name);
        mapped.frame_count = std::max(left.waveform.frame_count, right.waveform.frame_count);
        audio_internal::apply_sampler_mapping(result, audio_internal::map_a_series_sampler_metadata(mapped));
    } else {
        result.warnings.push_back(
            {"wav_stereo_sample_metadata_omitted",
             "Stereo Sample " + sample.display_name +
                 " members disagree on pitch, playback, or loop metadata; WAV smpl and inst chunks were omitted"});
    }
    add_coarse_tune_warning(sample, result);
    return result;
}

} // namespace axk::audio_export_detail
