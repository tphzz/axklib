#include "session_state.hpp"

#include <algorithm>
#include <format>
#include <numeric>
#include <ranges>
#include <utility>

#include "axklib/audio.hpp"
#include "axklib/lookups.hpp"
#include "image_sessions_internal.hpp"

namespace axk::app::formats::a_series {

Result<PcmMember> SessionState::prepare_member(std::string object_id, std::string role,
                                               const CurrentSbnkMember *sample_member,
                                               PcmReadWindow read_window) const {
    const auto snapshot = snapshots_by_id.find(object_id);
    if (snapshot == snapshots_by_id.end())
        return std::unexpected(session_error("object_not_found", "Wave Data object does not exist"));
    const auto *smpl = std::get_if<CurrentSmpl>(&snapshot->second->object.payload);
    if (smpl == nullptr)
        return std::unexpected(session_error("audition_unsupported", "audition requires SMPL Wave Data"));
    if (const auto profile = validate_smpl_pcm_transfer_control(*smpl); !profile)
        return std::unexpected(session_error("audition_unsupported", profile.error().message));
    if (smpl->stored_segment_offset != 0U || smpl->stored_segment_bytes != smpl->stored_pcm_bytes) {
        return std::unexpected(session_error(
            "companion_disks_required",
            "Wave Data continues on another sampler disk. Add extracted companion disk folders to audition it."));
    }
    if (smpl->sample_rate.value == 0U || smpl->stored_pcm_bytes == 0U)
        return std::unexpected(session_error("audition_unsupported", "Wave Data contains no playable PCM"));
    if (smpl->stored_sample_width_bytes.value != 1U && smpl->stored_sample_width_bytes.value != 2U)
        return std::unexpected(session_error("audition_unsupported", "Wave Data sample width is unsupported"));
    if (smpl->stored_pcm_bytes % smpl->stored_sample_width_bytes.value != 0U)
        return std::unexpected(
            session_error("invalid_audio_range", "Wave Data PCM size is not aligned to its sample width"));
    const auto physical_frame_count = smpl->stored_pcm_bytes / smpl->stored_sample_width_bytes.value;
    const auto playback_first_frame = sample_member ? sample_member->wave_start_frame : smpl->wave_start_frame.value;
    const auto playback_frame_count =
        sample_member ? sample_member->wave_length_frames : smpl->wave_length_frames.value;
    const auto window_owner = sample_member == nullptr ? "Wave Data" : "Sample";
    if (playback_frame_count == 0U) {
        return std::unexpected(
            session_error("audition_unsupported", std::format("{} playback window is empty", window_owner)));
    }
    if (playback_first_frame > physical_frame_count ||
        playback_frame_count > physical_frame_count - playback_first_frame) {
        return std::unexpected(session_error(
            "invalid_audio_range", std::format("{} playback window exceeds the stored Wave Data PCM", window_owner)));
    }
    const auto used_first_frame = read_window == PcmReadWindow::playback ? playback_first_frame : 0U;
    const auto used_frame_count = read_window == PcmReadWindow::playback ? playback_frame_count : physical_frame_count;
    return PcmMember{.object_id = std::move(object_id),
                     .role = std::move(role),
                     .output_width = smpl->stored_sample_width_bytes.value,
                     .stored_frame_count = physical_frame_count,
                     .playback_start_frame = playback_first_frame,
                     .playback_length_frames = playback_frame_count,
                     .physical_first_frame = used_first_frame,
                     .frame_count = used_frame_count};
}

Result<PcmSource> SessionState::prepare_source(std::string_view object_id, PcmReadWindow read_window,
                                               std::span<const ImageRelationshipItem> relationships) const {
    const auto snapshot = snapshots_by_id.find(std::string{object_id});
    if (snapshot == snapshots_by_id.end())
        return std::unexpected(session_error("object_not_found", "image object does not exist"));
    struct PendingMember {
        std::string object_id;
        std::string role;
        const CurrentSbnkMember *sample_member{};
    };
    std::vector<PendingMember> pending_members;
    const CurrentSbnk *sample = nullptr;
    if (std::holds_alternative<CurrentSmpl>(snapshot->second->object.payload)) {
        pending_members.push_back({std::string{object_id}, "MONO", nullptr});
    } else if (std::holds_alternative<CurrentSbnk>(snapshot->second->object.payload)) {
        sample = &std::get<CurrentSbnk>(snapshot->second->object.payload);
        const auto append_member = [&](std::string_view relationship_type, std::string role,
                                       const CurrentSbnkMember &member) -> Result<void> {
            std::optional<std::string> resolved_id;
            for (const auto &relationship : relationships) {
                if (relationship.source_object_id != object_id || relationship.type != relationship_type ||
                    !relationship.target_object_id || relationship.quality != "KNOWN") {
                    continue;
                }
                const auto target = snapshots_by_id.find(*relationship.target_object_id);
                if (target != snapshots_by_id.end() &&
                    std::holds_alternative<CurrentSmpl>(target->second->object.payload)) {
                    if (resolved_id && *resolved_id != *relationship.target_object_id)
                        return std::unexpected(session_error(
                            "audition_relationship_ambiguous",
                            "Sample audition requires one confirmed linked Wave Data object per member lane"));
                    resolved_id = *relationship.target_object_id;
                }
            }
            if (!resolved_id && source_reference.kind == ImageSourceKind::directory) {
                return std::unexpected(session_error("companion_disks_required",
                                                     "Wave Data continues on another sampler disk. Add "
                                                     "extracted companion disk folders to audition it."));
            }
            if (!resolved_id)
                return std::unexpected(
                    session_error("audition_relationship_ambiguous",
                                  "Sample audition requires one confirmed linked Wave Data object per member lane"));
            pending_members.push_back({std::move(*resolved_id), std::move(role), &member});
            return {};
        };
        auto left = append_member("SBNK_LEFT_MEMBER_TO_SMPL", "LEFT", sample->left);
        if (!left)
            return std::unexpected(left.error());
        if (sample->right_slot_present) {
            if (!sample->right)
                return std::unexpected(
                    session_error("audition_relationship_ambiguous", "Sample right member metadata is missing"));
            auto right = append_member("SBNK_RIGHT_MEMBER_TO_SMPL", "RIGHT", *sample->right);
            if (!right)
                return std::unexpected(right.error());
        }
    } else {
        return std::unexpected(session_error("audition_unsupported", "audition requires SMPL or SBNK content"));
    }

    PcmSource source;
    source.loop_mode =
        sample ? sample->loop_mode : std::get<CurrentSmpl>(snapshot->second->object.payload).loop_mode.value;
    source.loop_mode_label =
        sample ? sample->loop_mode_label : std::get<CurrentSmpl>(snapshot->second->object.payload).loop_mode_label;
    for (auto &pending : pending_members) {
        auto member =
            prepare_member(std::move(pending.object_id), std::move(pending.role), pending.sample_member, read_window);
        if (!member)
            return std::unexpected(member.error());
        const auto &member_snapshot = *snapshots_by_id.at(member->object_id);
        const auto &smpl = std::get<CurrentSmpl>(member_snapshot.object.payload);
        const auto sample_rate = pending.sample_member ? pending.sample_member->sample_rate : smpl.sample_rate.value;
        if (sample_rate == 0U)
            return std::unexpected(session_error("audition_unsupported", "Sample playback rate is zero"));
        member->sample_rate = sample_rate;
        member->source_loop_start =
            pending.sample_member ? pending.sample_member->loop_start_frame : smpl.loop_start_frame.value;
        member->source_loop_length =
            pending.sample_member ? pending.sample_member->loop_length_frames : smpl.loop_length_frames.value;
        if (member->source_loop_start < member->playback_start_frame) {
            source.warnings.emplace_back(
                std::format("{} loop starts before its playback window; playback will use one-shot mode",
                            pending.sample_member ? "Sample" : "Wave Data"));
            source.loop_mode = 0U;
            source.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, 0);
        } else {
            member->loop_start = read_window == PcmReadWindow::playback
                                     ? member->source_loop_start - member->playback_start_frame
                                     : member->source_loop_start;
            member->loop_length = member->source_loop_length;
        }
        if ((source.loop_mode == 1U || source.loop_mode == 2U) &&
            (member->loop_length == 0U || member->loop_start >= member->frame_count ||
             member->loop_length > member->frame_count - member->loop_start)) {
            source.warnings.emplace_back("Invalid loop bounds; playback will use one-shot mode");
            source.loop_mode = 0U;
            source.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, 0);
        }
        source.members.push_back(std::move(*member));
    }
    if (source.members.size() > 1U) {
        const auto &left = source.members.front();
        const auto different_format =
            std::ranges::any_of(source.members | std::views::drop(1U), [&](const auto &member) {
                return member.sample_rate != left.sample_rate || member.output_width != left.output_width;
            });
        if (different_format) {
            source.warnings.emplace_back(
                "Sample member formats differ; audition will normalize each lane independently");
        }
        if (source.loop_mode == 1U || source.loop_mode == 2U) {
            const auto equal_time = [](std::uint64_t left_frames, std::uint32_t left_rate, std::uint64_t right_frames,
                                       std::uint32_t right_rate) {
                const auto left_divisor = std::gcd(left_frames, static_cast<std::uint64_t>(left_rate));
                const auto right_divisor = std::gcd(right_frames, static_cast<std::uint64_t>(right_rate));
                return left_frames / left_divisor == right_frames / right_divisor &&
                       left_rate / left_divisor == right_rate / right_divisor;
            };
            const auto incompatible_loop =
                std::ranges::any_of(source.members | std::views::drop(1U), [&](const auto &member) {
                    return !equal_time(left.loop_start, left.sample_rate, member.loop_start, member.sample_rate) ||
                           !equal_time(left.loop_length, left.sample_rate, member.loop_length, member.sample_rate);
                });
            if (incompatible_loop) {
                source.warnings.emplace_back("Sample member loop timings differ; playback will use one-shot mode");
                source.loop_mode = 0U;
                source.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, 0);
            }
        }
    }
    if (source.loop_mode == 0U) {
        for (auto &member : source.members) {
            member.loop_start = 0U;
            member.loop_length = 0U;
        }
    }
    return source;
}

Result<std::vector<std::byte>> SessionState::read_member_pcm(const PcmMember &member, std::uint64_t first_frame,
                                                             std::size_t frame_count,
                                                             const CancellationToken &cancellation) const {
    const auto &snapshot = *snapshots_by_id.at(member.object_id);
    const auto &smpl = std::get<CurrentSmpl>(snapshot.object.payload);
    if (first_frame >= member.frame_count)
        return std::vector<std::byte>{};
    frame_count = static_cast<std::size_t>(std::min<std::uint64_t>(frame_count, member.frame_count - first_frame));
    const auto stored_width = smpl.stored_sample_width_bytes.value;
    const auto physical_first_frame = member.physical_first_frame + first_frame;
    auto stored = read_object_range(member.object_id, smpl.stored_pcm_offset + physical_first_frame * stored_width,
                                    frame_count * stored_width, cancellation);
    if (!stored)
        return std::unexpected(stored.error());
    if (stored_width == 2U) {
        for (std::size_t offset = 0U; offset < stored->size(); offset += 2U)
            std::swap((*stored)[offset], (*stored)[offset + 1U]);
    }
    return stored;
}

} // namespace axk::app::formats::a_series
