#include "axklib/application/image_sessions.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>

#include "axklib/application/secure_random.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/lookups.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

namespace {

axk::app::Error session_error(std::string code, std::string message, bool retryable = false) {
    return {std::move(code), std::move(message), {}, retryable};
}

axk::app::Error core_error(const axk::Error &error, const axk::app::FileRef &source) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = source.relative_path;
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "image_open_failed",
            error.message, std::move(context), false};
}

axk::app::Result<std::string> random_identifier(std::string_view prefix) {
    auto suffix = axk::app::secure_random_hex(16U);
    if (!suffix)
        return std::unexpected(suffix.error());
    return std::string{prefix} + std::move(*suffix);
}

std::string media_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12";
    case axk::MediaKind::iso9660:
        return "iso9660";
    case axk::MediaKind::standalone_object:
        return "standalone-object";
    }
    return "unknown";
}

std::string object_format_name(axk::ObjectFormat format) {
    switch (format) {
    case axk::ObjectFormat::current:
        return "current";
    case axk::ObjectFormat::alternating_byte:
        return "alternating-byte";
    case axk::ObjectFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

std::optional<std::string> mapped_id(const std::unordered_map<std::string, std::string> &ids, const std::string &key) {
    if (const auto found = ids.find(key); found != ids.end())
        return found->second;
    return std::nullopt;
}

std::optional<std::uint8_t> partition_index_from_node_id(std::string_view node_id) {
    constexpr std::string_view prefix = "partition:";
    if (!node_id.starts_with(prefix))
        return std::nullopt;
    node_id.remove_prefix(prefix.size());
    const auto separator = node_id.find(':');
    const auto value_text = node_id.substr(0U, separator);
    unsigned int value{};
    const auto parsed = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != value_text.data() + value_text.size() ||
        value > std::numeric_limits<std::uint8_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

} // namespace

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
        std::uint64_t physical_first_frame{};
        std::uint64_t frame_count{};
    };

    struct PcmSource {
        std::vector<PcmMember> members;
        std::uint32_t sample_rate{};
        std::uint16_t sample_width{};
        std::uint64_t frame_count{};
        std::uint8_t loop_mode{};
        std::string loop_mode_label;
        std::uint64_t loop_start{};
        std::uint64_t loop_length{};
        std::vector<std::string> warnings;
    };

    struct AuditionEntry {
        ImageAudition descriptor;
        PcmSource source;
        std::chrono::steady_clock::time_point last_access;
    };

    struct Session {
        std::string image_id;
        std::string owner_id;
        FileRef source;
        std::string format;
        std::vector<ImageContentItem> content;
        std::unordered_map<std::string, std::vector<std::size_t>> content_children;
        std::vector<ImageObjectItem> objects;
        std::unordered_map<std::string, std::size_t> object_indices_by_id;
        std::unordered_map<std::string, std::vector<std::size_t>> object_indices_by_type;
        std::unordered_map<std::string, std::vector<std::size_t>> object_indices_by_content_scope;
        std::vector<ImageRelationshipItem> relationships;
        std::vector<ImageValidationItem> validation;
        std::optional<MediaContainer> media;
        std::unordered_map<std::string, MediaObjectDescriptor> descriptors_by_id;
        std::unordered_map<std::string, ObjectSnapshot> snapshots_by_id;
        std::unordered_map<std::string, AuditionEntry> auditions;
        std::size_t root_count{};
        std::mutex access_mutex;
        std::chrono::steady_clock::time_point last_access;
        CursorSet content_cursors;
        CursorSet object_cursors;
        CursorSet relationship_cursors;
        CursorSet validation_cursors;
        PathReservationCoordinator::Lease path_lease;
    };

    const Sandbox &sandbox;
    std::size_t maximum_sessions;
    std::size_t maximum_page_size;
    std::chrono::seconds idle_retention;
    Clock clock;
    PathReservationCoordinator *path_reservations;
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;

    Implementation(const Sandbox &sandbox_value, std::size_t session_count, std::size_t page_size,
                   std::chrono::seconds retention, Clock now, PathReservationCoordinator *reservations)
        : sandbox(sandbox_value), maximum_sessions(std::max<std::size_t>(session_count, 1U)),
          maximum_page_size(std::max<std::size_t>(page_size, 1U)), idle_retention(retention), clock(std::move(now)),
          path_reservations(reservations) {}

    void cleanup_locked() {
        const auto now = clock();
        std::erase_if(sessions, [&](const auto &item) {
            const auto &session = item.second;
            const std::scoped_lock access_lock{session->access_mutex};
            return session->last_access + idle_retention <= now;
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

    Result<std::vector<std::byte>> read_object_range(const Session &session, std::string_view object_id,
                                                     std::uint64_t offset, std::size_t size,
                                                     const CancellationToken &cancellation) const {
        const auto snapshot = session.snapshots_by_id.find(std::string{object_id});
        const auto descriptor = session.descriptors_by_id.find(std::string{object_id});
        if (snapshot == session.snapshots_by_id.end() || descriptor == session.descriptors_by_id.end())
            return std::unexpected(session_error("object_not_found", "image object does not exist"));
        if (offset > descriptor->second.size || size > descriptor->second.size - offset)
            return std::unexpected(session_error("invalid_audio_range", "audio source range exceeds the object"));
        if (const auto *sfs = std::get_if<Container>(&session.media->storage())) {
            auto bytes =
                sfs->read_record_range(snapshot->second.partition, snapshot->second.sfs_id, offset, size, cancellation);
            if (!bytes)
                return std::unexpected(core_error(bytes.error(), session.source));
            return std::move(*bytes);
        }
        if (const auto *fat = std::get_if<FatImage>(&session.media->storage())) {
            const auto file = std::ranges::find(fat->files(), descriptor->second.logical_path, &FatFile::path);
            if (file == fat->files().end())
                return std::unexpected(session_error("object_not_found", "FAT12 object file does not exist"));
            auto bytes = fat->read_file_range(*file, offset, size, cancellation);
            if (!bytes)
                return std::unexpected(core_error(bytes.error(), session.source));
            return std::move(*bytes);
        }
        if (const auto *iso = std::get_if<IsoImage>(&session.media->storage())) {
            const auto file = std::ranges::find(iso->files(), descriptor->second.logical_path, &IsoFile::path);
            if (file == iso->files().end())
                return std::unexpected(session_error("object_not_found", "ISO object file does not exist"));
            auto bytes = iso->read_file_range(*file, offset, size, cancellation);
            if (!bytes)
                return std::unexpected(core_error(bytes.error(), session.source));
            return std::move(*bytes);
        }
        const auto &payload = snapshot->second.raw_payload;
        if (offset > payload.size() || size > payload.size() - offset)
            return std::unexpected(session_error("invalid_audio_range", "standalone object range is invalid"));
        return std::vector<std::byte>{payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                      payload.begin() + static_cast<std::ptrdiff_t>(offset + size)};
    }

    Result<PcmMember> prepare_member(Session &session, std::string object_id, std::string role,
                                     std::optional<std::uint64_t> first_frame,
                                     std::optional<std::uint64_t> requested_frame_count,
                                     const CancellationToken &cancellation) const {
        const auto snapshot = session.snapshots_by_id.find(object_id);
        if (snapshot == session.snapshots_by_id.end())
            return std::unexpected(session_error("object_not_found", "Wave Data object does not exist"));
        const auto *smpl = std::get_if<CurrentSmpl>(&snapshot->second.object.payload);
        if (smpl == nullptr)
            return std::unexpected(session_error("audition_unsupported", "audition requires SMPL Wave Data"));
        if (smpl->sample_rate.value == 0U || smpl->stored_pcm_bytes == 0U)
            return std::unexpected(session_error("audition_unsupported", "Wave Data contains no playable PCM"));
        if (smpl->stored_sample_width_bytes.value != 1U && smpl->stored_sample_width_bytes.value != 2U)
            return std::unexpected(session_error("audition_unsupported", "Wave Data sample width is unsupported"));
        if (smpl->stored_pcm_bytes % smpl->stored_sample_width_bytes.value != 0U)
            return std::unexpected(
                session_error("invalid_audio_range", "Wave Data PCM size is not aligned to its sample width"));
        const auto physical_frame_count = smpl->stored_pcm_bytes / smpl->stored_sample_width_bytes.value;
        const auto used_first_frame = first_frame.value_or(0U);
        const auto used_frame_count = requested_frame_count.value_or(physical_frame_count);
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
        return PcmMember{std::move(object_id), std::move(role),  alternating,
                         output_width,         used_first_frame, used_frame_count};
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
                        !relationship.target_object_id || relationship.quality != "Known") {
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
        for (auto &pending : pending_members) {
            const auto first_frame = pending.sample_member
                                         ? std::optional<std::uint64_t>{pending.sample_member->wave_start_frame}
                                         : std::nullopt;
            const auto frame_count = pending.sample_member
                                         ? std::optional<std::uint64_t>{pending.sample_member->wave_length_frames}
                                         : std::nullopt;
            auto member = prepare_member(session, std::move(pending.object_id), std::move(pending.role), first_frame,
                                         frame_count, cancellation);
            if (!member)
                return std::unexpected(member.error());
            const auto &member_snapshot = session.snapshots_by_id.at(member->object_id);
            const auto &smpl = std::get<CurrentSmpl>(member_snapshot.object.payload);
            const auto sample_rate =
                pending.sample_member ? pending.sample_member->sample_rate : smpl.sample_rate.value;
            if (sample_rate == 0U)
                return std::unexpected(session_error("audition_unsupported", "Sample playback rate is zero"));
            if (source.members.empty()) {
                source.sample_rate = sample_rate;
                source.sample_width = member->output_width;
                source.loop_mode = sample ? sample->loop_mode : smpl.loop_mode.value;
                source.loop_mode_label = sample ? sample->loop_mode_label : smpl.loop_mode_label;
                if (pending.sample_member) {
                    if (pending.sample_member->loop_start_frame < pending.sample_member->wave_start_frame) {
                        source.warnings.emplace_back(
                            "Sample loop starts before its playback window; playback will use one-shot mode");
                        source.loop_mode = 0U;
                        source.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, 0);
                    } else {
                        source.loop_start =
                            pending.sample_member->loop_start_frame - pending.sample_member->wave_start_frame;
                        source.loop_length = pending.sample_member->loop_length_frames;
                    }
                } else {
                    source.loop_start = smpl.loop_start_frame.value;
                    source.loop_length = smpl.loop_length_frames.value;
                }
            } else if (source.sample_rate != sample_rate || source.sample_width != member->output_width) {
                return std::unexpected(
                    session_error("audition_stereo_incompatible",
                                  "linked Wave Data must have matching sample rates and decoded sample widths"));
            } else if (pending.sample_member && source.loop_mode != 0U) {
                const auto loop_start =
                    pending.sample_member->loop_start_frame >= pending.sample_member->wave_start_frame
                        ? pending.sample_member->loop_start_frame - pending.sample_member->wave_start_frame
                        : std::numeric_limits<std::uint64_t>::max();
                if (loop_start != source.loop_start ||
                    pending.sample_member->loop_length_frames != source.loop_length) {
                    source.warnings.emplace_back("Sample member loop windows differ; playback will use one-shot mode");
                    source.loop_mode = 0U;
                    source.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, 0);
                    source.loop_start = 0U;
                    source.loop_length = 0U;
                }
            }
            source.frame_count = std::max(source.frame_count, member->frame_count);
            source.members.push_back(std::move(*member));
        }
        if ((source.loop_mode == 1U || source.loop_mode == 2U) &&
            (source.loop_length == 0U || source.loop_start >= source.frame_count ||
             source.loop_length > source.frame_count - source.loop_start)) {
            source.warnings.emplace_back("Invalid loop bounds; playback will use one-shot mode");
            source.loop_mode = 0U;
            source.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, 0);
            source.loop_start = 0U;
            source.loop_length = 0U;
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

    Result<std::vector<std::byte>> read_pcm(Session &session, const PcmSource &source, std::uint64_t first_frame,
                                            std::size_t frame_count, const CancellationToken &cancellation) const {
        std::vector<std::vector<std::byte>> members;
        members.reserve(source.members.size());
        for (const auto &member : source.members) {
            auto pcm = read_member_pcm(session, member, first_frame, frame_count, cancellation);
            if (!pcm)
                return std::unexpected(pcm.error());
            members.push_back(std::move(*pcm));
        }
        const auto channels = source.members.size();
        const auto block = channels * source.sample_width;
        std::vector<std::byte> result(frame_count * block, source.sample_width == 1U ? std::byte{0x80} : std::byte{0});
        for (std::size_t frame = 0U; frame < frame_count; ++frame) {
            for (std::size_t channel = 0U; channel < channels; ++channel) {
                const auto source_offset = frame * source.sample_width;
                if (source_offset + source.sample_width > members[channel].size())
                    continue;
                const auto destination = (frame * channels + channel) * source.sample_width;
                std::ranges::copy_n(members[channel].begin() + static_cast<std::ptrdiff_t>(source_offset),
                                    source.sample_width, result.begin() + static_cast<std::ptrdiff_t>(destination));
            }
        }
        return result;
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

axk::app::ImageSessionManager::ImageSessionManager(const Sandbox &sandbox, std::size_t maximum_sessions,
                                                   std::size_t maximum_page_size, std::chrono::seconds idle_retention,
                                                   Clock clock, PathReservationCoordinator *path_reservations)
    : implementation_(std::make_unique<Implementation>(sandbox, maximum_sessions, maximum_page_size, idle_retention,
                                                       std::move(clock), path_reservations)) {}

axk::app::ImageSessionManager::~ImageSessionManager() = default;
axk::app::ImageSessionManager::ImageSessionManager(ImageSessionManager &&) noexcept = default;
axk::app::ImageSessionManager &axk::app::ImageSessionManager::operator=(ImageSessionManager &&) noexcept = default;

axk::app::Result<axk::app::ImageSessionSummary>
axk::app::ImageSessionManager::open(const FileRef &source, std::string owner_id,
                                    const CancellationToken &cancellation) {
    if (owner_id.empty())
        return std::unexpected(session_error("invalid_owner", "image session owner is required"));
    PathReservationCoordinator::Lease path_lease;
    if (implementation_->path_reservations != nullptr) {
        auto acquired = implementation_->path_reservations->try_acquire({source, PathAccessMode::shared});
        if (!acquired)
            return std::unexpected(acquired.error());
        path_lease = std::move(*acquired);
    }
    const auto file = implementation_->sandbox.open_file(source);
    if (!file)
        return std::unexpected(file.error());
    const auto media = axk::open_media(file->reader, std::filesystem::path{file->filename}, cancellation);
    if (!media)
        return std::unexpected(core_error(media.error(), source));
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U,
                                                cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), source));
    auto graph = axk::build_relationship_graph(inventory->catalog);
    auto tree = axk::build_content_tree(*media, inventory->catalog, graph);
    std::unordered_map<std::uint8_t, std::string> partition_names;
    if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
        partition_names.reserve(sfs->partitions().size());
        for (const auto &partition : sfs->partitions())
            partition_names.emplace(partition.index.value, partition.name);
    }

    auto session = std::make_shared<Implementation::Session>();
    session->owner_id = std::move(owner_id);
    session->source = source;
    session->format = media_kind_name(media->kind());
    session->root_count = tree.roots.size();
    session->last_access = implementation_->clock();
    session->path_lease = std::move(path_lease);

    std::unordered_map<std::string, std::string> object_ids;
    object_ids.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        auto identifier = random_identifier("object-");
        if (!identifier)
            return std::unexpected(identifier.error());
        object_ids.emplace(object.key, std::move(*identifier));
    }
    for (const auto &descriptor : inventory->objects)
        session->descriptors_by_id.emplace(object_ids.at(descriptor.key), descriptor);

    session->objects.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        ImageObjectItem item;
        item.id = object_ids.at(object.key);
        item.type = object.object.header.raw_type;
        item.name = object.object.header.name;
        item.format = object_format_name(object.object.format);
        item.stored_size_bytes = session->descriptors_by_id.at(item.id).size;
        if (object.placement) {
            item.partition_index = object.partition.value;
            item.partition_name = object.placement->partition_name;
            item.volume_name = object.placement->volume_name;
            item.category_name = object.placement->category_name;
            item.entry_name = object.placement->entry_name;
        }
        if (const auto *waveform = std::get_if<axk::CurrentSmpl>(&object.object.payload)) {
            const auto stored_width = waveform->stored_sample_width_bytes.value;
            item.waveform =
                WaveformMetadata{.sample_rate = waveform->sample_rate.value,
                                 .sample_width_bytes = waveform->stored_sample_width_bytes.value,
                                 .root_key = waveform->root_key.value,
                                 .fine_tune_cents = waveform->fine_tune_cents.value,
                                 .loop_mode = waveform->loop_mode.value,
                                 .loop_mode_label = waveform->loop_mode_label,
                                 .frame_count = stored_width == 0U ? 0U : waveform->stored_pcm_bytes / stored_width,
                                 .loop_start_frame = waveform->loop_start_frame.value,
                                 .loop_length_frames = waveform->loop_length_frames.value};
        }
        session->object_indices_by_id[item.id] = session->objects.size();
        session->object_indices_by_type[item.type].push_back(session->objects.size());
        session->objects.push_back(std::move(item));
    }

    session->relationships.reserve(graph.relationships.size());
    for (const auto &relationship : graph.relationships) {
        const auto source_id = mapped_id(object_ids, relationship.source_key);
        if (!source_id)
            continue;
        ImageRelationshipItem item;
        auto relationship_id = random_identifier("relationship-");
        if (!relationship_id)
            return std::unexpected(relationship_id.error());
        item.id = std::move(*relationship_id);
        item.source_object_id = *source_id;
        if (relationship.target_key)
            item.target_object_id = mapped_id(object_ids, *relationship.target_key);
        for (const auto &candidate : relationship.candidate_keys) {
            if (const auto candidate_id = mapped_id(object_ids, candidate))
                item.candidate_object_ids.push_back(*candidate_id);
        }
        item.type = relationship.type;
        item.quality = axk::relationship_quality_name(relationship.quality);
        item.basis = relationship.basis;
        item.notes = relationship.notes;
        item.assignment_index = relationship.assignment_index;
        item.assignment_name = relationship.assignment_name;
        item.assignment_state = axk::assignment_state_name(relationship.assignment_state);
        item.receive_channel_display = relationship.receive_channel_display;
        session->relationships.push_back(std::move(item));
    }

    const auto append_content =
        [&](const auto &self, const axk::ContentNode &node, const std::optional<std::string> &parent_id,
            std::size_t depth,
            std::optional<std::uint8_t> inherited_partition_index) -> axk::app::Result<std::vector<std::size_t>> {
        auto generated_id = random_identifier("content-");
        if (!generated_id)
            return std::unexpected(generated_id.error());
        const auto id = std::move(*generated_id);
        const auto partition_index =
            node.node_type == "partition" ? partition_index_from_node_id(node.node_id) : inherited_partition_index;
        const auto canonical_name = [&]() -> std::string {
            if (node.node_type == "partition" && partition_index) {
                if (const auto found = partition_names.find(*partition_index); found != partition_names.end())
                    return found->second;
            }
            return node.display_name;
        }();
        ImageContentItem item{.id = id,
                              .parent_id = parent_id,
                              .depth = depth,
                              .partition_index = partition_index,
                              .kind = node.node_type,
                              .name = canonical_name,
                              .display_name = node.display_name,
                              .child_count = node.children.size(),
                              .object_id = std::nullopt,
                              .object_type = std::nullopt,
                              .quality = std::string{axk::relationship_quality_name(node.quality)},
                              .basis = node.basis,
                              .notes = node.notes,
                              .details = node.details};
        if (!node.object_key.empty())
            item.object_id = mapped_id(object_ids, node.object_key);
        if (!node.object_type.empty())
            item.object_type = node.object_type;
        const auto item_index = session->content.size();
        session->content.push_back(std::move(item));
        session->content_children[parent_id.value_or("")].push_back(item_index);
        session->content_children.try_emplace(id);
        std::vector<std::size_t> scoped_indices;
        if (session->content[item_index].object_id) {
            if (const auto found = session->object_indices_by_id.find(*session->content[item_index].object_id);
                found != session->object_indices_by_id.end()) {
                scoped_indices.push_back(found->second);
            }
        }
        for (const auto &child : node.children) {
            auto appended = self(self, child, id, depth + 1U, partition_index);
            if (!appended)
                return std::unexpected(appended.error());
            scoped_indices.insert(scoped_indices.end(), appended->begin(), appended->end());
        }
        std::ranges::sort(scoped_indices);
        const auto unique_end = std::ranges::unique(scoped_indices).begin();
        scoped_indices.erase(unique_end, scoped_indices.end());
        session->object_indices_by_content_scope.emplace(id, scoped_indices);
        return scoped_indices;
    };
    session->content_children.try_emplace("");
    for (const auto &root : tree.roots) {
        if (auto appended = append_content(append_content, root, std::nullopt, 0U, std::nullopt); !appended)
            return std::unexpected(appended.error());
    }

    const auto append_validation = [&](std::string code, std::string severity, std::string message,
                                       std::string sampler_path, std::optional<std::string> object_id) {
        session->validation.push_back(
            {std::move(code), std::move(severity), std::move(message), std::move(sampler_path), std::move(object_id)});
    };
    for (const auto &issue : tree.issues) {
        append_validation(issue.code, issue.severity, issue.message, issue.sampler_path,
                          issue.object_key.empty() ? std::nullopt : mapped_id(object_ids, issue.object_key));
    }
    for (const auto &issue : media->validation_issues())
        append_validation(issue.code, "warning", issue.message, issue.sampler_path, std::nullopt);
    if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
        const auto report = axk::validate_semantics(*sfs, inventory->catalog, graph);
        for (const auto &issue : report.issues) {
            std::string severity;
            switch (issue.severity) {
            case axk::ValidationSeverity::info:
                severity = "info";
                break;
            case axk::ValidationSeverity::warning:
                severity = "warning";
                break;
            case axk::ValidationSeverity::error:
                severity = "error";
                break;
            }
            append_validation(issue.code, std::move(severity), issue.message, issue.sampler_path,
                              issue.object_key.empty() ? std::nullopt : mapped_id(object_ids, issue.object_key));
        }
    }
    for (auto &object : inventory->catalog.objects) {
        const auto identifier = object_ids.at(object.key);
        session->snapshots_by_id.emplace(identifier, std::move(object));
    }
    session->media.emplace(std::move(*media));

    do {
        auto image_id = random_identifier("image-");
        if (!image_id)
            return std::unexpected(image_id.error());
        session->image_id = std::move(*image_id);
        const std::scoped_lock lock{implementation_->mutex};
        implementation_->cleanup_locked();
        if (implementation_->sessions.size() >= implementation_->maximum_sessions)
            return std::unexpected(
                session_error("image_capacity_exhausted", "image session capacity is exhausted", true));
        if (!implementation_->sessions.contains(session->image_id)) {
            implementation_->sessions.emplace(session->image_id, session);
            break;
        }
    } while (true);
    return inspect(session->image_id, session->owner_id);
}

axk::app::Result<axk::app::ImageSessionSummary> axk::app::ImageSessionManager::inspect(std::string_view image_id,
                                                                                       std::string_view owner_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    ImageValidationSummary validation;
    for (const auto &issue : (*session)->validation) {
        if (issue.severity == "error")
            ++validation.error_count;
        else if (issue.severity == "warning")
            ++validation.warning_count;
        else
            ++validation.info_count;
    }
    std::vector<std::string> available_operations{"images.content",           "images.objects", "images.relationships",
                                                  "images.validation.issues", "images.preview", "auditions.prepare"};
    const auto source_metadata =
        implementation_->sandbox.metadata((*session)->source.root_id, (*session)->source.relative_path);
    if ((*session)->format == "sfs" && source_metadata && source_metadata->writable) {
        available_operations.emplace_back("images.alter.volumes");
        available_operations.emplace_back("images.alter.partitions");
    }
    return ImageSessionSummary{.image_id = (*session)->image_id,
                               .source = (*session)->source,
                               .format = (*session)->format,
                               .available_operations = std::move(available_operations),
                               .root_count = (*session)->root_count,
                               .object_count = (*session)->objects.size(),
                               .relationship_count = (*session)->relationships.size(),
                               .validation = validation};
}

axk::app::Result<void> axk::app::ImageSessionManager::close(std::string_view image_id, std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
    const auto found = implementation_->sessions.find(std::string{image_id});
    if (found == implementation_->sessions.end())
        return {};
    if (found->second->owner_id != owner_id)
        return std::unexpected(session_error("image_not_found", "image session does not exist"));
    implementation_->sessions.erase(found);
    return {};
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageContentItem>>
axk::app::ImageSessionManager::content(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                                       std::optional<std::string_view> cursor,
                                       std::optional<std::string_view> parent_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto scope = parent_id.value_or("");
    const auto children = (*session)->content_children.find(std::string{scope});
    if (children == (*session)->content_children.end())
        return std::unexpected(session_error("content_not_found", "content parent does not exist"));
    return implementation_->page((*session)->content, (*session)->content_cursors, limit, cursor, scope,
                                 &children->second);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageObjectItem>> axk::app::ImageSessionManager::objects(
    std::string_view image_id, std::string_view owner_id, std::size_t limit, std::optional<std::string_view> cursor,
    std::optional<std::string_view> object_type, std::optional<std::string_view> content_scope_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (!object_type && !content_scope_id)
        return implementation_->page((*session)->objects, (*session)->object_cursors, limit, cursor);

    const auto scope =
        "content:" + std::string{content_scope_id.value_or("")} + "\ntype:" + std::string{object_type.value_or("")};
    const std::vector<std::size_t> *indices = nullptr;
    static const std::vector<std::size_t> empty;
    if (content_scope_id) {
        const auto found = (*session)->object_indices_by_content_scope.find(std::string{*content_scope_id});
        if (found == (*session)->object_indices_by_content_scope.end())
            return std::unexpected(session_error("content_not_found", "content scope does not exist"));
        indices = &found->second;
    } else {
        const auto found = (*session)->object_indices_by_type.find(std::string{*object_type});
        indices = found == (*session)->object_indices_by_type.end() ? &empty : &found->second;
    }

    std::vector<std::size_t> filtered;
    if (content_scope_id && object_type) {
        filtered.reserve(indices->size());
        std::ranges::copy_if(*indices, std::back_inserter(filtered),
                             [&](const std::size_t index) { return (*session)->objects[index].type == *object_type; });
        indices = &filtered;
    }
    return implementation_->page((*session)->objects, (*session)->object_cursors, limit, cursor, scope, indices);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageRelationshipItem>>
axk::app::ImageSessionManager::relationships(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                                             std::optional<std::string_view> cursor, ImageRelationshipFilter filter) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (!filter.content_scope_id && !filter.source_object_id && !filter.target_object_id && !filter.relationship_type)
        return implementation_->page((*session)->relationships, (*session)->relationship_cursors, limit, cursor);

    const auto scope = "content:" + std::string{filter.content_scope_id.value_or("")} +
                       "\nsource:" + std::string{filter.source_object_id.value_or("")} +
                       "\ntarget:" + std::string{filter.target_object_id.value_or("")} +
                       "\ntype:" + std::string{filter.relationship_type.value_or("")};
    const std::vector<std::size_t> *content_indices = nullptr;
    if (filter.content_scope_id) {
        const auto found = (*session)->object_indices_by_content_scope.find(std::string{*filter.content_scope_id});
        if (found == (*session)->object_indices_by_content_scope.end())
            return std::unexpected(session_error("content_not_found", "content scope does not exist"));
        content_indices = &found->second;
    }

    std::vector<std::size_t> indices;
    indices.reserve((*session)->relationships.size());
    for (std::size_t index = 0U; index < (*session)->relationships.size(); ++index) {
        const auto &item = (*session)->relationships[index];
        if (filter.source_object_id && item.source_object_id != *filter.source_object_id)
            continue;
        if (filter.target_object_id && (!item.target_object_id || *item.target_object_id != *filter.target_object_id))
            continue;
        if (filter.relationship_type && item.type != *filter.relationship_type)
            continue;
        if (content_indices != nullptr) {
            const auto source = (*session)->object_indices_by_id.find(item.source_object_id);
            if (source == (*session)->object_indices_by_id.end() ||
                !std::ranges::binary_search(*content_indices, source->second)) {
                continue;
            }
        }
        indices.push_back(index);
    }
    return implementation_->page((*session)->relationships, (*session)->relationship_cursors, limit, cursor, scope,
                                 &indices);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageValidationItem>>
axk::app::ImageSessionManager::validation_issues(std::string_view image_id, std::string_view owner_id,
                                                 std::size_t limit, std::optional<std::string_view> cursor) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    return implementation_->page((*session)->validation, (*session)->validation_cursors, limit, cursor);
}

void axk::app::ImageSessionManager::cleanup() {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
}

axk::app::Result<axk::app::ImageWaveformPreview>
axk::app::ImageSessionManager::preview(std::string_view image_id, std::string_view owner_id, std::string_view object_id,
                                       std::size_t bin_count, const CancellationToken &cancellation) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (bin_count == 0U || bin_count > 4096U)
        return std::unexpected(session_error("invalid_preview", "preview bin count is outside the configured range"));
    auto source = implementation_->prepare_source(**session, object_id, cancellation);
    if (!source)
        return std::unexpected(source.error());
    ImageWaveformPreview result{.object_id = std::string{object_id}, .frame_count = source->frame_count, .lanes = {}};
    result.lanes.reserve(source->members.size());
    constexpr std::size_t chunk_frames = 16U * 1024U;
    for (const auto &member : source->members) {
        const auto used_bins = static_cast<std::size_t>(std::min<std::uint64_t>(bin_count, member.frame_count));
        ImageWaveformPreviewLane lane{
            .role = member.role, .source_object_id = member.object_id, .frame_count = member.frame_count, .bins = {}};
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
                                                std::string_view object_id, const CancellationToken &cancellation) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    auto source = implementation_->prepare_source(**session, object_id, cancellation);
    if (!source)
        return std::unexpected(source.error());
    const auto data_size = source->frame_count * source->members.size() * source->sample_width;
    if (data_size > std::numeric_limits<std::uint32_t>::max() - 36U)
        return std::unexpected(session_error("audition_unsupported", "audition exceeds the RIFF/WAVE size limit"));
    auto audition_id = random_identifier("audition-");
    if (!audition_id)
        return std::unexpected(audition_id.error());
    ImageAudition descriptor{.audition_id = *audition_id,
                             .object_id = std::string{object_id},
                             .sample_rate = source->sample_rate,
                             .channels = static_cast<std::uint16_t>(source->members.size()),
                             .sample_width_bytes = source->sample_width,
                             .frame_count = source->frame_count,
                             .wav_size_bytes = 44U + data_size,
                             .loop_mode = source->loop_mode,
                             .loop_mode_label = source->loop_mode_label,
                             .loop_start_frame = source->loop_start,
                             .loop_length_frames = source->loop_length,
                             .warnings = source->warnings};
    const std::scoped_lock lock{(*session)->access_mutex};
    const auto now = implementation_->clock();
    std::erase_if((*session)->auditions,
                  [&](const auto &entry) { return entry.second.last_access + std::chrono::minutes{10} <= now; });
    if ((*session)->auditions.size() >= 256U)
        return std::unexpected(session_error("audition_capacity_exhausted", "audition capacity is exhausted", true));
    (*session)->auditions.emplace(*audition_id, Implementation::AuditionEntry{descriptor, std::move(*source), now});
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
        for (const auto &[unused, candidate] : implementation_->sessions) {
            static_cast<void>(unused);
            if (candidate->owner_id != owner_id)
                continue;
            const std::scoped_lock access_lock{candidate->access_mutex};
            const auto found = candidate->auditions.find(std::string{audition_id});
            if (found != candidate->auditions.end()) {
                session = candidate;
                break;
            }
        }
    }
    if (!session)
        return std::unexpected(session_error("audition_not_found", "audition does not exist"));
    const std::scoped_lock access_lock{session->access_mutex};
    const auto found = session->auditions.find(std::string{audition_id});
    if (found == session->auditions.end())
        return std::unexpected(session_error("audition_not_found", "audition does not exist"));
    auto &entry = found->second;
    session->last_access = implementation_->clock();
    entry.last_access = session->last_access;
    const auto total_size = entry.descriptor.wav_size_bytes;
    if (offset > total_size || size > total_size - offset)
        return std::unexpected(session_error("invalid_audio_range", "audio byte range exceeds the audition"));

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
    const auto data_size = static_cast<std::uint32_t>(total_size - header.size());
    const auto block = static_cast<std::uint16_t>(entry.descriptor.channels * entry.descriptor.sample_width_bytes);
    write_tag(0U, "RIFF");
    le32(4U, data_size + 36U);
    write_tag(8U, "WAVEfmt ");
    le32(16U, 16U);
    le16(20U, 1U);
    le16(22U, entry.descriptor.channels);
    le32(24U, entry.descriptor.sample_rate);
    le32(28U, entry.descriptor.sample_rate * block);
    le16(32U, block);
    le16(34U, static_cast<std::uint16_t>(entry.descriptor.sample_width_bytes * 8U));
    write_tag(36U, "data");
    le32(40U, data_size);

    ImageAuditionRange result{.total_size = total_size, .bytes = {}};
    result.bytes.reserve(size);
    const auto header_count =
        offset < header.size() ? static_cast<std::size_t>(std::min<std::uint64_t>(size, header.size() - offset)) : 0U;
    if (header_count > 0U) {
        result.bytes.insert(result.bytes.end(), header.begin() + static_cast<std::ptrdiff_t>(offset),
                            header.begin() + static_cast<std::ptrdiff_t>(offset + header_count));
    }
    if (result.bytes.size() < size) {
        const auto data_offset = offset + result.bytes.size() - header.size();
        const auto data_count = size - result.bytes.size();
        const auto first_frame = data_offset / block;
        const auto first_byte = static_cast<std::size_t>(data_offset % block);
        const auto frame_count = (first_byte + data_count + block - 1U) / block;
        auto pcm = implementation_->read_pcm(*session, entry.source, first_frame, frame_count, cancellation);
        if (!pcm)
            return std::unexpected(pcm.error());
        result.bytes.insert(result.bytes.end(), pcm->begin() + static_cast<std::ptrdiff_t>(first_byte),
                            pcm->begin() + static_cast<std::ptrdiff_t>(first_byte + data_count));
    }
    return result;
}

axk::app::Result<void> axk::app::ImageSessionManager::delete_audition(std::string_view audition_id,
                                                                      std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
    for (const auto &[unused, session] : implementation_->sessions) {
        static_cast<void>(unused);
        if (session->owner_id != owner_id)
            continue;
        const std::scoped_lock access_lock{session->access_mutex};
        if (session->auditions.erase(std::string{audition_id}) != 0U)
            return {};
    }
    return {};
}
