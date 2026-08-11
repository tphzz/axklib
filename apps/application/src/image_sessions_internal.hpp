#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "axklib/application/image_sessions.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/lookups.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"

namespace axk::app::image_sessions_internal {

Error session_error(std::string code, std::string message, bool retryable = false);
Error core_error(const axk::Error &error, const ImageSourceRef &source);
Result<std::string> random_identifier(std::string_view prefix);
std::string media_kind_name(axk::MediaKind kind);
std::string fold_ascii(std::string_view value);
Result<std::vector<DirectoryEntry>> list_bounded_directory(const Sandbox &sandbox, const DirectoryRef &reference,
                                                           std::size_t maximum_entries);

struct CompanionSegments {
    std::vector<ImageSourceRef> sources;
    std::vector<FileRef> files;
};

Result<CompanionSegments> append_required_companion_wave_data(const Sandbox &sandbox, const ImageSourceRef &source,
                                                              const axk::AxkObjectDirectory &primary,
                                                              const std::vector<ImageSourceRef> &companion_sources,
                                                              std::vector<axk::AxkObjectDirectoryEntry> &entries,
                                                              std::vector<std::function<Result<void>()>> &verifiers);
Result<std::vector<DirectoryRef>> immediate_sibling_directories(const Sandbox &sandbox, const ImageSourceRef &source);
std::string object_format_name(axk::ObjectFormat format);
std::string object_type_name(axk::ObjectType type);
std::string relationship_quality_wire_name(axk::RelationshipQuality quality);
std::optional<std::string> mapped_id(const std::unordered_map<std::string, std::string> &ids, const std::string &key);
std::optional<std::uint8_t> partition_index_from_node_id(std::string_view node_id);
std::uint64_t record_cluster_count(const axk::Container &container, const axk::ObjectSnapshot &object);

} // namespace axk::app::image_sessions_internal

using axk::app::image_sessions_internal::append_required_companion_wave_data;
using axk::app::image_sessions_internal::core_error;
using axk::app::image_sessions_internal::fold_ascii;
using axk::app::image_sessions_internal::immediate_sibling_directories;
using axk::app::image_sessions_internal::mapped_id;
using axk::app::image_sessions_internal::media_kind_name;
using axk::app::image_sessions_internal::object_format_name;
using axk::app::image_sessions_internal::object_type_name;
using axk::app::image_sessions_internal::partition_index_from_node_id;
using axk::app::image_sessions_internal::random_identifier;
using axk::app::image_sessions_internal::record_cluster_count;
using axk::app::image_sessions_internal::relationship_quality_wire_name;
using axk::app::image_sessions_internal::session_error;

struct axk::app::ImageSessionManager::Implementation {
    struct CursorSet {
        struct Position {
            std::size_t offset{};
            std::string scope;
        };

        std::mutex mutex;
        std::unordered_map<std::string, Position> positions;
        std::unordered_map<std::string, std::string> cursors;
    };

    struct PcmMember {
        std::string object_id;
        std::string role;
        bool alternating_byte{};
        std::uint16_t output_width{};
        std::uint32_t sample_rate{};
        std::uint64_t physical_first_frame{};
        std::uint64_t frame_count{};
        std::uint64_t loop_start{};
        std::uint64_t loop_length{};
    };

    struct PcmSource {
        std::vector<PcmMember> members;
        std::uint8_t loop_mode{};
        std::string loop_mode_label;
        std::vector<std::string> warnings;
    };

    struct AuditionEntry {
        ImageAudition descriptor;
        std::vector<PcmSource> sources;
        std::chrono::steady_clock::time_point last_access;
    };

    struct Session {
        std::string image_id;
        std::string owner_id;
        ImageSourceRef source;
        std::vector<ImageSourceRef> companion_sources;
        std::optional<ImageFloppySetSummary> floppy_set;
        std::string format;
        std::vector<ImageContentItem> content;
        std::unordered_map<std::string, std::vector<std::size_t>> content_children;
        std::vector<ImageObjectItem> objects;
        std::unordered_map<std::string, std::size_t> object_indices_by_id;
        std::unordered_map<std::string, std::vector<std::size_t>> object_indices_by_type;
        std::unordered_map<std::string, std::vector<std::size_t>> object_indices_by_content_scope;
        std::vector<ImageRelationshipItem> relationships;
        std::vector<ImageValidationItem> validation;
        std::shared_ptr<const RandomAccessReader> source_reader;
        std::function<Result<void>()> verify_source_unchanged;
        std::string target_snapshot_id;
        std::optional<MediaContainer> media;
        std::unordered_map<std::string, MediaObjectDescriptor> descriptors_by_id;
        std::unordered_map<std::string, ObjectSnapshot> snapshots_by_id;
        std::unordered_map<std::string, axk::WaveformStatus> waveform_status_by_id;
        std::unordered_map<std::string, std::uint64_t> waveform_cluster_counts_by_id;
        std::vector<CatalogIssue> catalog_issues;
        std::unordered_map<std::string, AuditionEntry> auditions;
        std::size_t root_count{};
        std::uint64_t revision{1U};
        bool mutating{};
        std::mutex access_mutex;
        std::optional<std::unique_lock<std::mutex>> mutation_guard;
        std::chrono::steady_clock::time_point last_access;
        CursorSet content_cursors;
        CursorSet object_cursors;
        CursorSet relationship_cursors;
        CursorSet validation_cursors;
        PathReservationCoordinator::Lease path_lease;
        PathReservationCoordinator::Lease companion_path_lease;
    };

    const Sandbox &sandbox;
    std::size_t maximum_sessions;
    std::size_t maximum_page_size;
    std::uint64_t maximum_audition_bundle_bytes;
    std::size_t maximum_audition_clips;
    std::chrono::seconds idle_retention;
    Clock clock;
    PathReservationCoordinator *path_reservations;
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    std::unordered_map<std::string, std::weak_ptr<Session>> audition_sessions;
    std::size_t pending_sessions{};

    struct PendingAdmission {
        explicit PendingAdmission(Implementation &implementation);
        PendingAdmission(const PendingAdmission &) = delete;
        PendingAdmission &operator=(const PendingAdmission &) = delete;
        ~PendingAdmission();

        bool promote(const std::shared_ptr<Session> &session);

      private:
        Implementation *implementation_;
        bool active_{true};
    };

    Implementation(const Sandbox &sandbox_value, std::size_t session_count, std::size_t page_size,
                   std::chrono::seconds retention, Clock now, PathReservationCoordinator *reservations,
                   std::uint64_t audition_bundle_bytes, std::size_t audition_clips)
        : sandbox(sandbox_value), maximum_sessions(std::max<std::size_t>(session_count, 1U)),
          maximum_page_size(std::max<std::size_t>(page_size, 1U)),
          maximum_audition_bundle_bytes(std::max<std::uint64_t>(audition_bundle_bytes, 45U)),
          maximum_audition_clips(std::max<std::size_t>(audition_clips, 1U)), idle_retention(retention),
          clock(std::move(now)), path_reservations(reservations) {}

    void cleanup_locked() {
        const auto now = clock();
        std::erase_if(sessions, [&](const auto &item) {
            const auto &session = item.second;
            const std::unique_lock access_lock{session->access_mutex, std::try_to_lock};
            if (!access_lock)
                return false;
            return session->last_access + idle_retention <= now;
        });
        std::erase_if(audition_sessions, [](const auto &item) { return item.second.expired(); });
    }

    Result<std::unique_ptr<PendingAdmission>> reserve_session() {
        const std::scoped_lock lock{mutex};
        cleanup_locked();
        if (sessions.size() + pending_sessions >= maximum_sessions)
            return std::unexpected(
                session_error("image_capacity_exhausted", "image session capacity is exhausted", true));
        ++pending_sessions;
        return std::make_unique<PendingAdmission>(*this);
    }

    void remove_auditions_for(const std::shared_ptr<Session> &session) {
        const std::scoped_lock lock{mutex};
        std::erase_if(audition_sessions, [&](const auto &item) {
            const auto owner = item.second.lock();
            return !owner || owner == session;
        });
    }

    Result<std::shared_ptr<Session>> owned(std::string_view image_id, std::string_view owner_id) {
        std::shared_ptr<Session> result;
        {
            const std::scoped_lock lock{mutex};
            cleanup_locked();
            const auto found = sessions.find(std::string{image_id});
            if (found == sessions.end() || found->second->owner_id != owner_id)
                return std::unexpected(session_error("image_not_found", "image session does not exist"));
            result = found->second;
        }
        {
            const std::scoped_lock lock{result->access_mutex};
            result->last_access = clock();
        }
        return result;
    }

    static void preserve_object_ids(const Session &current, Session &fresh) {
        const auto stable_key = [](const Session &session, const ObjectSnapshot &snapshot) {
            if (session.format == "sfs") {
                return std::format("{}:{}:{}", snapshot.partition.value, snapshot.sfs_id.value,
                                   snapshot.object.header.raw_type);
            }
            if (session.format == "axk-object-directory") {
                const auto separator = snapshot.key.find(':');
                const auto logical_path = separator == std::string::npos
                                              ? std::string_view{snapshot.key}
                                              : std::string_view{snapshot.key}.substr(separator + 1U);
                auto path = axk::text::path_from_utf8(logical_path);
                const auto filename =
                    path && !path->filename().empty() ? axk::text::path_to_utf8(path->filename()) : snapshot.key;
                return std::format("{}:{}:{}", snapshot.object.header.raw_type, fold_ascii(filename),
                                   snapshot.object.header.name);
            }
            return snapshot.key;
        };
        std::unordered_map<std::string, std::string> current_ids_by_key;
        current_ids_by_key.reserve(current.snapshots_by_id.size());
        for (const auto &[id, snapshot] : current.snapshots_by_id)
            current_ids_by_key.emplace(stable_key(current, snapshot), id);
        std::unordered_map<std::string, std::string> remapped;
        remapped.reserve(fresh.snapshots_by_id.size());
        for (const auto &[id, snapshot] : fresh.snapshots_by_id) {
            if (const auto found = current_ids_by_key.find(stable_key(fresh, snapshot));
                found != current_ids_by_key.end()) {
                remapped.emplace(id, found->second);
            } else {
                remapped.emplace(id, id);
            }
        }
        const auto map_id = [&](std::string &id) {
            if (const auto found = remapped.find(id); found != remapped.end())
                id = found->second;
        };
        for (auto &object : fresh.objects)
            map_id(object.id);
        for (auto &content : fresh.content) {
            if (content.object_id)
                map_id(*content.object_id);
        }
        for (auto &relationship : fresh.relationships) {
            map_id(relationship.source_object_id);
            if (relationship.target_object_id)
                map_id(*relationship.target_object_id);
            for (auto &candidate : relationship.candidate_object_ids)
                map_id(candidate);
        }
        for (auto &issue : fresh.validation) {
            if (issue.object_id)
                map_id(*issue.object_id);
        }
        decltype(fresh.descriptors_by_id) descriptors;
        descriptors.reserve(fresh.descriptors_by_id.size());
        for (auto &[id, descriptor] : fresh.descriptors_by_id) {
            auto mapped = id;
            map_id(mapped);
            descriptors.emplace(std::move(mapped), std::move(descriptor));
        }
        fresh.descriptors_by_id = std::move(descriptors);
        decltype(fresh.snapshots_by_id) snapshots;
        snapshots.reserve(fresh.snapshots_by_id.size());
        for (auto &[id, snapshot] : fresh.snapshots_by_id) {
            auto mapped = id;
            map_id(mapped);
            snapshots.emplace(std::move(mapped), std::move(snapshot));
        }
        fresh.snapshots_by_id = std::move(snapshots);
        decltype(fresh.waveform_status_by_id) waveform_statuses;
        waveform_statuses.reserve(fresh.waveform_status_by_id.size());
        for (auto &[id, status] : fresh.waveform_status_by_id) {
            auto mapped = id;
            map_id(mapped);
            waveform_statuses.emplace(std::move(mapped), status);
        }
        fresh.waveform_status_by_id = std::move(waveform_statuses);
        decltype(fresh.waveform_cluster_counts_by_id) waveform_cluster_counts;
        waveform_cluster_counts.reserve(fresh.waveform_cluster_counts_by_id.size());
        for (auto &[id, count] : fresh.waveform_cluster_counts_by_id) {
            auto mapped = id;
            map_id(mapped);
            waveform_cluster_counts.emplace(std::move(mapped), count);
        }
        fresh.waveform_cluster_counts_by_id = std::move(waveform_cluster_counts);
        fresh.object_indices_by_id.clear();
        fresh.object_indices_by_type.clear();
        for (std::size_t index = 0U; index < fresh.objects.size(); ++index) {
            fresh.object_indices_by_id.emplace(fresh.objects[index].id, index);
            fresh.object_indices_by_type[fresh.objects[index].type].push_back(index);
        }
    }

    static void adopt_refreshed_state(Session &current, Session &fresh) {
        current.source = std::move(fresh.source);
        current.companion_sources = std::move(fresh.companion_sources);
        current.floppy_set = std::move(fresh.floppy_set);
        current.format = std::move(fresh.format);
        current.content = std::move(fresh.content);
        current.content_children = std::move(fresh.content_children);
        current.objects = std::move(fresh.objects);
        current.object_indices_by_id = std::move(fresh.object_indices_by_id);
        current.object_indices_by_type = std::move(fresh.object_indices_by_type);
        current.object_indices_by_content_scope = std::move(fresh.object_indices_by_content_scope);
        current.relationships = std::move(fresh.relationships);
        current.validation = std::move(fresh.validation);
        current.source_reader = std::move(fresh.source_reader);
        current.verify_source_unchanged = std::move(fresh.verify_source_unchanged);
        current.target_snapshot_id = std::move(fresh.target_snapshot_id);
        current.media = std::move(fresh.media);
        current.descriptors_by_id = std::move(fresh.descriptors_by_id);
        current.snapshots_by_id = std::move(fresh.snapshots_by_id);
        current.waveform_status_by_id = std::move(fresh.waveform_status_by_id);
        current.waveform_cluster_counts_by_id = std::move(fresh.waveform_cluster_counts_by_id);
        current.catalog_issues = std::move(fresh.catalog_issues);
        current.companion_path_lease = std::move(fresh.companion_path_lease);
        current.auditions.clear();
        current.root_count = fresh.root_count;
        current.last_access = fresh.last_access;
        {
            const std::scoped_lock lock{current.content_cursors.mutex};
            current.content_cursors.positions.clear();
            current.content_cursors.cursors.clear();
        }
        {
            const std::scoped_lock lock{current.object_cursors.mutex};
            current.object_cursors.positions.clear();
            current.object_cursors.cursors.clear();
        }
        {
            const std::scoped_lock lock{current.relationship_cursors.mutex};
            current.relationship_cursors.positions.clear();
            current.relationship_cursors.cursors.clear();
        }
        {
            const std::scoped_lock lock{current.validation_cursors.mutex};
            current.validation_cursors.positions.clear();
            current.validation_cursors.cursors.clear();
        }
    }

    Result<std::vector<std::byte>> read_object_range(const Session &session, std::string_view object_id,
                                                     std::uint64_t offset, std::size_t size,
                                                     const CancellationToken &cancellation) const;

    Result<PcmMember> prepare_member(Session &session, std::string object_id, std::string role,
                                     const CurrentSbnkMember *sample_member,
                                     const CancellationToken &cancellation) const {
        const auto snapshot = session.snapshots_by_id.find(object_id);
        if (snapshot == session.snapshots_by_id.end())
            return std::unexpected(session_error("object_not_found", "Wave Data object does not exist"));
        const auto *smpl = std::get_if<CurrentSmpl>(&snapshot->second.object.payload);
        if (smpl == nullptr)
            return std::unexpected(session_error("audition_unsupported", "audition requires SMPL Wave Data"));
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
        const auto used_first_frame = sample_member == nullptr ? 0U : sample_member->wave_start_frame;
        const auto used_frame_count =
            sample_member == nullptr ? physical_frame_count : sample_member->wave_length_frames;
        if (used_frame_count == 0U)
            return std::unexpected(session_error("audition_unsupported", "Sample playback window is empty"));
        if (used_first_frame > physical_frame_count || used_frame_count > physical_frame_count - used_first_frame)
            return std::unexpected(
                session_error("invalid_audio_range", "Sample playback window exceeds the linked Wave Data"));
        bool alternating = smpl->stored_sample_width_bytes.value == 2U && smpl->stored_pcm_bytes >= 2U;
        constexpr std::size_t chunk_size = 64U * 1024U;
        for (std::uint64_t offset = 0U; alternating && offset < smpl->stored_pcm_bytes; offset += chunk_size) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, smpl->stored_pcm_bytes - offset));
            auto bytes = read_object_range(session, object_id, smpl->stored_pcm_offset + offset, count, cancellation);
            if (!bytes)
                return std::unexpected(bytes.error());
            for (std::size_t index = 1U; index < bytes->size(); index += 2U) {
                const auto absolute = offset + index;
                const auto expected = absolute % 4U == 1U ? 0x55U : 0xaaU;
                if (std::to_integer<std::uint8_t>((*bytes)[index]) != expected) {
                    alternating = false;
                    break;
                }
            }
        }
        const auto output_width = static_cast<std::uint16_t>(alternating ? 1U : smpl->stored_sample_width_bytes.value);
        return PcmMember{.object_id = std::move(object_id),
                         .role = std::move(role),
                         .alternating_byte = alternating,
                         .output_width = output_width,
                         .physical_first_frame = used_first_frame,
                         .frame_count = used_frame_count};
    }

    Result<PcmSource> prepare_source(Session &session, std::string_view object_id,
                                     const CancellationToken &cancellation) const {
        const auto snapshot = session.snapshots_by_id.find(std::string{object_id});
        if (snapshot == session.snapshots_by_id.end())
            return std::unexpected(session_error("object_not_found", "image object does not exist"));
        struct PendingMember {
            std::string object_id;
            std::string role;
            const CurrentSbnkMember *sample_member{};
        };
        std::vector<PendingMember> pending_members;
        const CurrentSbnk *sample = nullptr;
        if (std::holds_alternative<CurrentSmpl>(snapshot->second.object.payload)) {
            pending_members.push_back({std::string{object_id}, "MONO", nullptr});
        } else if (std::holds_alternative<CurrentSbnk>(snapshot->second.object.payload)) {
            sample = &std::get<CurrentSbnk>(snapshot->second.object.payload);
            const auto append_member = [&](std::string_view relationship_type, std::string role,
                                           const CurrentSbnkMember &member) -> Result<void> {
                std::optional<std::string> resolved_id;
                for (const auto &relationship : session.relationships) {
                    if (relationship.source_object_id != object_id || relationship.type != relationship_type ||
                        !relationship.target_object_id || relationship.quality != "KNOWN") {
                        continue;
                    }
                    const auto target = session.snapshots_by_id.find(*relationship.target_object_id);
                    if (target != session.snapshots_by_id.end() &&
                        std::holds_alternative<CurrentSmpl>(target->second.object.payload)) {
                        if (resolved_id && *resolved_id != *relationship.target_object_id)
                            return std::unexpected(session_error(
                                "audition_relationship_ambiguous",
                                "Sample audition requires one confirmed linked Wave Data object per member lane"));
                        resolved_id = *relationship.target_object_id;
                    }
                }
                if (!resolved_id && session.source.kind == ImageSourceKind::axk_object_directory) {
                    return std::unexpected(session_error("companion_disks_required",
                                                         "Wave Data continues on another sampler disk. Add "
                                                         "extracted companion disk folders to audition it."));
                }
                if (!resolved_id)
                    return std::unexpected(session_error(
                        "audition_relationship_ambiguous",
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
            sample ? sample->loop_mode : std::get<CurrentSmpl>(snapshot->second.object.payload).loop_mode.value;
        source.loop_mode_label =
            sample ? sample->loop_mode_label : std::get<CurrentSmpl>(snapshot->second.object.payload).loop_mode_label;
        for (auto &pending : pending_members) {
            auto member = prepare_member(session, std::move(pending.object_id), std::move(pending.role),
                                         pending.sample_member, cancellation);
            if (!member)
                return std::unexpected(member.error());
            const auto &member_snapshot = session.snapshots_by_id.at(member->object_id);
            const auto &smpl = std::get<CurrentSmpl>(member_snapshot.object.payload);
            const auto sample_rate =
                pending.sample_member ? pending.sample_member->sample_rate : smpl.sample_rate.value;
            if (sample_rate == 0U)
                return std::unexpected(session_error("audition_unsupported", "Sample playback rate is zero"));
            member->sample_rate = sample_rate;
            if (pending.sample_member) {
                if (pending.sample_member->loop_start_frame < pending.sample_member->wave_start_frame) {
                    source.warnings.emplace_back(
                        "Sample loop starts before its playback window; playback will use one-shot mode");
                    source.loop_mode = 0U;
                    source.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, 0);
                } else {
                    member->loop_start =
                        pending.sample_member->loop_start_frame - pending.sample_member->wave_start_frame;
                    member->loop_length = pending.sample_member->loop_length_frames;
                }
            } else {
                member->loop_start = smpl.loop_start_frame.value;
                member->loop_length = smpl.loop_length_frames.value;
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
                const auto equal_time = [](std::uint64_t left_frames, std::uint32_t left_rate,
                                           std::uint64_t right_frames, std::uint32_t right_rate) {
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

    Result<std::vector<std::byte>> read_member_pcm(Session &session, const PcmMember &member, std::uint64_t first_frame,
                                                   std::size_t frame_count,
                                                   const CancellationToken &cancellation) const {
        const auto &snapshot = session.snapshots_by_id.at(member.object_id);
        const auto &smpl = std::get<CurrentSmpl>(snapshot.object.payload);
        if (first_frame >= member.frame_count)
            return std::vector<std::byte>{};
        frame_count = static_cast<std::size_t>(std::min<std::uint64_t>(frame_count, member.frame_count - first_frame));
        const auto stored_width = smpl.stored_sample_width_bytes.value;
        const auto physical_first_frame = member.physical_first_frame + first_frame;
        auto stored =
            read_object_range(session, member.object_id, smpl.stored_pcm_offset + physical_first_frame * stored_width,
                              frame_count * stored_width, cancellation);
        if (!stored)
            return std::unexpected(stored.error());
        if (member.alternating_byte) {
            std::vector<std::byte> result;
            result.reserve(frame_count);
            for (std::size_t offset = 0U; offset < stored->size(); offset += 2U) {
                result.push_back(
                    static_cast<std::byte>((std::to_integer<std::uint8_t>((*stored)[offset]) + 128U) & 0xffU));
            }
            return result;
        }
        if (stored_width == 2U) {
            for (std::size_t offset = 0U; offset < stored->size(); offset += 2U)
                std::swap((*stored)[offset], (*stored)[offset + 1U]);
        }
        return stored;
    }

    template <typename Item>
    Result<ImagePage<Item>> page(const std::vector<Item> &items, CursorSet &cursors, std::size_t limit,
                                 std::optional<std::string_view> cursor, std::string_view scope = {},
                                 const std::vector<std::size_t> *indices = nullptr) const {
        if (limit == 0U || limit > maximum_page_size)
            return std::unexpected(session_error("invalid_page", "page limit is outside the configured range"));
        std::size_t offset{};
        {
            const std::scoped_lock lock{cursors.mutex};
            if (cursor) {
                const auto found = cursors.positions.find(std::string{*cursor});
                if (found == cursors.positions.end() || found->second.scope != scope)
                    return std::unexpected(session_error("invalid_cursor", "page cursor is invalid or stale"));
                offset = found->second.offset;
            }
        }
        const auto item_count = indices == nullptr ? items.size() : indices->size();
        if (offset > item_count)
            return std::unexpected(session_error("invalid_cursor", "page cursor is invalid or stale"));
        const auto count = std::min(limit, item_count - offset);
        ImagePage<Item> result;
        result.total_count = item_count;
        result.items.reserve(count);
        for (std::size_t index = offset; index < offset + count; ++index)
            result.items.push_back(indices == nullptr ? items[index] : items[indices->at(index)]);
        if (offset + count < item_count) {
            const auto next_offset = offset + count;
            const auto cursor_key = std::string{scope} + '\0' + std::to_string(next_offset);
            const std::scoped_lock lock{cursors.mutex};
            auto [found, inserted] = cursors.cursors.emplace(cursor_key, std::string{});
            if (inserted) {
                do {
                    auto identifier = random_identifier("cursor-");
                    if (!identifier)
                        return std::unexpected(identifier.error());
                    found->second = std::move(*identifier);
                } while (cursors.positions.contains(found->second));
                cursors.positions.emplace(found->second, CursorSet::Position{next_offset, std::string{scope}});
            }
            result.next_cursor = found->second;
        }
        return result;
    }
};
