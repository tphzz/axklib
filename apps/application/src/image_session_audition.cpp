#include "image_sessions_internal.hpp"

#include <format>
#include <unordered_set>

axk::app::Result<axk::app::ImageWaveformPreview>
axk::app::ImageSessionManager::preview(std::string_view image_id, std::string_view owner_id, std::string_view object_id,
                                       std::size_t bin_count, const CancellationToken &cancellation) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (bin_count == 0U || bin_count > 4096U)
        return std::unexpected(session_error("invalid_preview", "preview bin count is outside the configured range"));
    auto source =
        implementation_->prepare_source(**session, object_id, Implementation::PcmReadWindow::stored_pcm, cancellation);
    if (!source)
        return std::unexpected(source.error());
    ImageWaveformPreview result{.object_id = std::string{object_id}, .lanes = {}};
    result.lanes.reserve(source->members.size());
    constexpr std::size_t chunk_frames = 16U * 1024U;
    for (const auto &member : source->members) {
        const auto used_bins = static_cast<std::size_t>(std::min<std::uint64_t>(bin_count, member.frame_count));
        ImageWaveformPreviewLane lane{.role = member.role,
                                      .source_object_id = member.object_id,
                                      .sample_rate = member.sample_rate,
                                      .stored_frame_count = member.stored_frame_count,
                                      .playback_start_frame = member.playback_start_frame,
                                      .playback_length_frames = member.playback_length_frames,
                                      .loop_start_frame = member.source_loop_start,
                                      .loop_length_frames = member.source_loop_length,
                                      .bins = {}};
        lane.bins.assign(used_bins,
                         {std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::min()});
        for (std::uint64_t first = 0U; first < member.frame_count; first += chunk_frames) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(chunk_frames, member.frame_count - first));
            auto pcm = implementation_->read_member_pcm(**session, member, first, count, cancellation);
            if (!pcm)
                return std::unexpected(pcm.error());
            for (std::size_t frame = 0U; frame < count; ++frame) {
                const auto absolute_frame = first + frame;
                const auto bin = static_cast<std::size_t>(absolute_frame * used_bins / member.frame_count);
                const auto offset = frame * member.output_width;
                std::int32_t value{};
                if (member.output_width == 1U) {
                    value = std::to_integer<std::uint8_t>((*pcm)[offset]) - 128;
                } else {
                    const auto low = std::to_integer<std::uint8_t>((*pcm)[offset]);
                    const auto high = std::to_integer<std::uint8_t>((*pcm)[offset + 1U]);
                    value = static_cast<std::int16_t>(static_cast<std::uint16_t>(low | (high << 8U)));
                }
                lane.bins[bin].minimum = std::min(lane.bins[bin].minimum, value);
                lane.bins[bin].maximum = std::max(lane.bins[bin].maximum, value);
            }
        }
        result.lanes.push_back(std::move(lane));
    }
    return result;
}

axk::app::Result<axk::app::ImageAudition>
axk::app::ImageSessionManager::prepare_audition(std::string_view image_id, std::string_view owner_id,
                                                const std::vector<std::string> &object_ids,
                                                const CancellationToken &cancellation) {
    if (object_ids.empty() || object_ids.size() > implementation_->maximum_audition_clips) {
        return std::unexpected(
            session_error("invalid_request", std::format("audition must contain between 1 and {} unique objects",
                                                         implementation_->maximum_audition_clips)));
    }
    std::unordered_set<std::string_view> unique_ids;
    unique_ids.reserve(object_ids.size());
    if (!std::ranges::all_of(object_ids, [&](const auto &object_id) {
            return !object_id.empty() && unique_ids.insert(object_id).second;
        })) {
        return std::unexpected(session_error("invalid_request", "audition object IDs must be nonempty and unique"));
    }
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    ImageAudition descriptor;
    std::vector<Implementation::PcmSource> sources;
    descriptor.clips.reserve(object_ids.size());
    sources.reserve(object_ids.size());
    for (const auto &object_id : object_ids) {
        if (cancellation.is_cancelled())
            return std::unexpected(session_error("operation_cancelled", "operation cancelled"));
        const auto attach_object_context = [&](Error &error) {
            error.context.object_id = object_id;
            if (const auto index = (*session)->object_indices_by_id.find(object_id);
                index != (*session)->object_indices_by_id.end()) {
                const auto &object = (*session)->objects[index->second];
                error.context.object_type = object.type;
                error.context.object_name = object.name;
            }
        };
        const auto contextual_error = [&](std::string code, std::string message) {
            auto error = session_error(std::move(code), std::move(message));
            attach_object_context(error);
            return error;
        };
        auto source = implementation_->prepare_source(**session, object_id, Implementation::PcmReadWindow::playback,
                                                      cancellation);
        if (!source) {
            auto error = std::move(source.error());
            attach_object_context(error);
            return std::unexpected(std::move(error));
        }
        ImageAuditionClip clip{.object_id = object_id,
                               .loop_mode = source->loop_mode,
                               .loop_mode_label = source->loop_mode_label,
                               .warnings = source->warnings,
                               .lanes = {}};
        clip.lanes.reserve(source->members.size());
        for (const auto &member : source->members) {
            constexpr auto maximum_wave_data = std::numeric_limits<std::uint32_t>::max() - 36U;
            if (member.frame_count > maximum_wave_data / member.output_width) {
                return std::unexpected(
                    contextual_error("audition_unsupported", "audition lane exceeds the RIFF/WAVE size limit"));
            }
            const auto data_size = member.frame_count * member.output_width;
            const auto wav_size = 44U + data_size;
            if (wav_size > implementation_->maximum_audition_bundle_bytes - descriptor.content_size_bytes) {
                return std::unexpected(
                    contextual_error("audition_too_large", "audition bundle exceeds the configured byte limit"));
            }
            clip.lanes.push_back(ImageAuditionLane{.role = member.role,
                                                   .source_object_id = member.object_id,
                                                   .sample_rate = member.sample_rate,
                                                   .sample_width_bytes = member.output_width,
                                                   .frame_count = member.frame_count,
                                                   .content_offset_bytes = descriptor.content_size_bytes,
                                                   .wav_size_bytes = wav_size,
                                                   .loop_start_frame = member.loop_start,
                                                   .loop_length_frames = member.loop_length});
            descriptor.content_size_bytes += wav_size;
        }
        descriptor.clips.push_back(std::move(clip));
        sources.push_back(std::move(*source));
    }
    auto audition_id = random_identifier("audition-");
    if (!audition_id)
        return std::unexpected(audition_id.error());
    descriptor.audition_id = *audition_id;
    const std::scoped_lock lock{(*session)->access_mutex};
    const auto now = implementation_->clock();
    std::vector<std::string> expired_auditions;
    std::erase_if((*session)->auditions, [&](const auto &entry) {
        const auto expired = entry.second.last_access + std::chrono::minutes{10} <= now;
        if (expired)
            expired_auditions.push_back(entry.first);
        return expired;
    });
    if ((*session)->auditions.size() >= 256U)
        return std::unexpected(session_error("audition_capacity_exhausted", "audition capacity is exhausted", true));
    (*session)->auditions.emplace(*audition_id, Implementation::AuditionEntry{descriptor, std::move(sources), now});
    {
        const std::scoped_lock manager_lock{implementation_->mutex};
        for (const auto &expired : expired_auditions)
            implementation_->audition_sessions.erase(expired);
        implementation_->audition_sessions.emplace(*audition_id, *session);
    }
    return descriptor;
}

axk::app::Result<axk::app::ImageAuditionRange>
axk::app::ImageSessionManager::audition_range(std::string_view audition_id, std::string_view owner_id,
                                              std::uint64_t offset, std::size_t size,
                                              const CancellationToken &cancellation) {
    std::shared_ptr<Implementation::Session> session;
    {
        const std::scoped_lock lock{implementation_->mutex};
        implementation_->cleanup_locked();
        const auto found = implementation_->audition_sessions.find(std::string{audition_id});
        if (found != implementation_->audition_sessions.end()) {
            session = found->second.lock();
            if (!session)
                implementation_->audition_sessions.erase(found);
        }
    }
    if (!session || session->owner_id != owner_id)
        return std::unexpected(session_error("audition_not_found", "audition does not exist"));
    const std::scoped_lock access_lock{session->access_mutex};
    const auto found = session->auditions.find(std::string{audition_id});
    if (found == session->auditions.end())
        return std::unexpected(session_error("audition_not_found", "audition does not exist"));
    auto &entry = found->second;
    session->last_access = implementation_->clock();
    entry.last_access = session->last_access;
    const auto total_size = entry.descriptor.content_size_bytes;
    if (offset > total_size || size > total_size - offset)
        return std::unexpected(session_error("invalid_audio_range", "audio byte range exceeds the audition"));
    ImageAuditionRange result{.total_size = total_size, .bytes = {}};
    result.bytes.reserve(size);
    const auto requested_end = offset + size;
    for (std::size_t clip_index = 0U; clip_index < entry.descriptor.clips.size(); ++clip_index) {
        const auto &clip = entry.descriptor.clips[clip_index];
        const auto &source = entry.sources[clip_index];
        for (std::size_t lane_index = 0U; lane_index < clip.lanes.size(); ++lane_index) {
            const auto &lane = clip.lanes[lane_index];
            const auto lane_end = lane.content_offset_bytes + lane.wav_size_bytes;
            if (lane_end <= offset || lane.content_offset_bytes >= requested_end)
                continue;
            std::array<std::byte, 44> header{};
            const auto write_tag = [&](std::size_t at, std::string_view text) {
                std::ranges::transform(text, header.begin() + static_cast<std::ptrdiff_t>(at),
                                       [](char value) { return static_cast<std::byte>(value); });
            };
            const auto le16 = [&](std::size_t at, std::uint16_t value) {
                header[at] = static_cast<std::byte>(value & 0xffU);
                header[at + 1U] = static_cast<std::byte>(value >> 8U);
            };
            const auto le32 = [&](std::size_t at, std::uint32_t value) {
                for (std::size_t index = 0U; index < 4U; ++index)
                    header[at + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
            };
            const auto data_size = static_cast<std::uint32_t>(lane.wav_size_bytes - header.size());
            write_tag(0U, "RIFF");
            le32(4U, data_size + 36U);
            write_tag(8U, "WAVEfmt ");
            le32(16U, 16U);
            le16(20U, 1U);
            le16(22U, 1U);
            le32(24U, lane.sample_rate);
            le32(28U, lane.sample_rate * lane.sample_width_bytes);
            le16(32U, lane.sample_width_bytes);
            le16(34U, static_cast<std::uint16_t>(lane.sample_width_bytes * 8U));
            write_tag(36U, "data");
            le32(40U, data_size);

            const auto local_offset = std::max(offset, lane.content_offset_bytes) - lane.content_offset_bytes;
            const auto local_end = std::min(requested_end, lane_end) - lane.content_offset_bytes;
            const auto header_end = std::min<std::uint64_t>(local_end, header.size());
            if (local_offset < header_end) {
                result.bytes.insert(result.bytes.end(), header.begin() + static_cast<std::ptrdiff_t>(local_offset),
                                    header.begin() + static_cast<std::ptrdiff_t>(header_end));
            }
            const auto data_begin = std::max<std::uint64_t>(local_offset, header.size());
            if (data_begin >= local_end)
                continue;
            const auto data_offset = data_begin - header.size();
            const auto data_count = static_cast<std::size_t>(local_end - data_begin);
            const auto first_frame = data_offset / lane.sample_width_bytes;
            const auto first_byte = static_cast<std::size_t>(data_offset % lane.sample_width_bytes);
            const auto frame_count = (first_byte + data_count + lane.sample_width_bytes - 1U) / lane.sample_width_bytes;
            auto pcm = implementation_->read_member_pcm(*session, source.members[lane_index], first_frame, frame_count,
                                                        cancellation);
            if (!pcm)
                return std::unexpected(pcm.error());
            result.bytes.insert(result.bytes.end(), pcm->begin() + static_cast<std::ptrdiff_t>(first_byte),
                                pcm->begin() + static_cast<std::ptrdiff_t>(first_byte + data_count));
        }
    }
    if (result.bytes.size() != size)
        return std::unexpected(session_error("invalid_audio_range", "audition bundle range is incomplete"));
    return result;
}

axk::app::Result<void> axk::app::ImageSessionManager::delete_audition(std::string_view audition_id,
                                                                      std::string_view owner_id) {
    std::shared_ptr<Implementation::Session> session;
    {
        const std::scoped_lock lock{implementation_->mutex};
        implementation_->cleanup_locked();
        const auto found = implementation_->audition_sessions.find(std::string{audition_id});
        if (found != implementation_->audition_sessions.end())
            session = found->second.lock();
    }
    if (!session || session->owner_id != owner_id)
        return {};
    const std::scoped_lock access_lock{session->access_mutex};
    session->auditions.erase(std::string{audition_id});
    const std::scoped_lock manager_lock{implementation_->mutex};
    implementation_->audition_sessions.erase(std::string{audition_id});
    return {};
}
