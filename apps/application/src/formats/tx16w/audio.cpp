#include "formats/tx16w/session_state.hpp"

#include <algorithm>
#include <variant>

namespace axk::app::formats::tx16w {

Result<native::PcmSource> SessionState::prepare_source(std::string_view object_id, native::PcmReadWindow) const {
    const auto found = document_->object(object_id);
    if (!found)
        return std::unexpected(Error{"object_not_found", found.error().message});
    const auto *header = std::get_if<axk::tx16w::WaveHeader>(&(*found)->value);
    if (!header)
        return std::unexpected(Error{"audition_unsupported", "Audition requires a decoded TX16W Wave"});
    const auto frames = static_cast<std::uint64_t>(header->attack_frames) + header->repeat_frames;
    if (frames == 0U || header->sample_rate == 0U)
        return std::unexpected(Error{"audition_unsupported", "TX16W Wave contains no playable PCM"});
    native::PcmSource source;
    source.looping = header->looped && header->repeat_frames > 0U;
    source.members.push_back(
        {.object_id = std::string{object_id},
         .role = "MONO",
         .output_width = 2U,
         .native_bit_depth = 12U,
         .sample_rate = header->sample_rate,
         .sample_rate_approximate = header->sample_rate_source == axk::tx16w::SampleRateSource::defaulted_33333,
         .stored_frame_count = frames,
         .playback_start_frame = 0U,
         .playback_length_frames = frames,
         .source_loop_start = source.looping ? header->attack_frames : 0U,
         .source_loop_length = source.looping ? header->repeat_frames : 0U,
         .physical_first_frame = 0U,
         .frame_count = frames,
         .loop_start = source.looping ? header->attack_frames : 0U,
         .loop_length = source.looping ? header->repeat_frames : 0U});
    if (header->sample_rate_source == axk::tx16w::SampleRateSource::defaulted_33333)
        source.warnings.emplace_back(
            "Sample rate is uncertain; audition uses the 33,333 Hz decoding default. Duration is approximate.");
    if (header->looped && !source.looping)
        source.warnings.emplace_back("Empty repeat region; audition uses one-shot playback.");
    return source;
}

Result<std::vector<std::byte>> SessionState::read_member_pcm(const native::PcmMember &member, std::uint64_t first_frame,
                                                             std::size_t frame_count,
                                                             const CancellationToken &cancellation) const {
    const auto frames = document_->read_wave_frames(member.object_id, first_frame, frame_count, cancellation);
    if (!frames)
        return std::unexpected(
            Error{frames.error().code == ErrorCode::operation_cancelled ? "operation_cancelled" : "audio_read_failed",
                  frames.error().message});
    std::vector<std::byte> bytes;
    bytes.reserve(frames->size() * 2U);
    for (const auto frame : *frames) {
        const auto value = static_cast<std::uint16_t>(frame);
        bytes.push_back(static_cast<std::byte>(value & 0xffU));
        bytes.push_back(static_cast<std::byte>(value >> 8U));
    }
    return bytes;
}

} // namespace axk::app::formats::tx16w
