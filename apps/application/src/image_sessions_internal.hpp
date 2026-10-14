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
#include "axklib/media.hpp"
#include "image_filesystem_internal.hpp"
#include "image_sources.hpp"
#include "native_document_admission.hpp"
#include "native_document_state.hpp"

namespace axk::app::image_sessions_internal {

Error session_error(std::string code, std::string message, bool retryable = false);
Error core_error(const axk::Error &error, const ImageSourceRef &source);
Result<std::string> random_identifier(std::string_view prefix);
std::string media_kind_name(axk::MediaKind kind);
std::string fold_ascii(std::string_view value);
Result<std::vector<DirectoryEntry>> list_bounded_directory(const Sandbox &sandbox, const DirectoryRef &reference,
                                                           std::size_t maximum_entries);

} // namespace axk::app::image_sessions_internal

using axk::app::image_sessions_internal::core_error;
using axk::app::image_sessions_internal::fold_ascii;
using axk::app::image_sessions_internal::media_kind_name;
using axk::app::image_sessions_internal::random_identifier;
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

    struct Document : NativeDocumentAdmission {
        CursorSet content_cursors;
        CursorSet object_cursors;
        CursorSet relationship_cursors;
        CursorSet validation_cursors;
    };

    struct Session {
        std::string image_id;
        std::string owner_id;
        AdmittedImageSources sources;
        std::vector<std::shared_ptr<Document>> documents;
        ImageDocumentDiscovery discovery;
        std::vector<std::string> selected_formats;
        std::shared_ptr<const detail::ImageFilesystemIndex> filesystem_index;
        std::uint64_t revision{1U};
        bool mutating{};
        bool invalidated{};
        std::mutex access_mutex;
        std::optional<std::unique_lock<std::mutex>> mutation_guard;
        std::chrono::steady_clock::time_point last_access;
        CursorSet document_cursors;
        std::string mutation_member_id;
    };

    const Sandbox &sandbox;
    std::size_t maximum_sessions;
    std::size_t maximum_page_size;
    std::uint64_t maximum_audition_bundle_bytes;
    std::size_t maximum_audition_clips;
    std::chrono::seconds idle_retention;
    Clock clock;
    PathReservationCoordinator *path_reservations;
    DeviceSessionManager *devices;
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    struct AuditionOwner {
        std::weak_ptr<Session> session;
        std::string document_id;
    };
    std::unordered_map<std::string, AuditionOwner> audition_sessions;
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
                   std::uint64_t audition_bundle_bytes, std::size_t audition_clips,
                   DeviceSessionManager *device_manager)
        : sandbox(sandbox_value), maximum_sessions(std::max<std::size_t>(session_count, 1U)),
          maximum_page_size(std::max<std::size_t>(page_size, 1U)),
          maximum_audition_bundle_bytes(std::max<std::uint64_t>(audition_bundle_bytes, 45U)),
          maximum_audition_clips(std::max<std::size_t>(audition_clips, 1U)), idle_retention(retention),
          clock(std::move(now)), path_reservations(reservations), devices(device_manager) {}

    void cleanup_locked() {
        const auto now = clock();
        std::erase_if(sessions, [&](const auto &item) {
            const auto &session = item.second;
            const std::unique_lock access_lock{session->access_mutex, std::try_to_lock};
            if (!access_lock)
                return false;
            return session->last_access + idle_retention <= now;
        });
        std::erase_if(audition_sessions, [](const auto &item) { return item.second.session.expired(); });
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
            const auto owner = item.second.session.lock();
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
            if (result->invalidated)
                return std::unexpected(
                    session_error("image_session_invalidated", "image session requires recovery and reopening"));
            result->last_access = clock();
        }
        return result;
    }

    static Result<std::shared_ptr<Document>> document(const Session &session, std::string_view id) {
        const auto found = std::ranges::find_if(session.documents, [&](const auto &value) { return value->id == id; });
        if (found == session.documents.end())
            return std::unexpected(session_error("document_not_found", "native document does not exist"));
        return *found;
    }

    static Result<std::shared_ptr<ImageSourceMember>> member(const Session &session, std::string_view id) {
        const auto found =
            std::ranges::find_if(session.sources.members, [&](const auto &value) { return value->id == id; });
        if (found == session.sources.members.end())
            return std::unexpected(session_error("source_member_not_found", "source member does not exist"));
        return *found;
    }

    Result<void> discover(Session &session, const CancellationToken &cancellation, ProgressSink *progress = nullptr,
                          diagnostic_sink *diagnostics = nullptr);
    Result<std::shared_ptr<const detail::ImageFilesystemIndex>> filesystem(Session &session) const;
    Result<ImageDocumentSummary> summarize(const Session &session, const Document &document) const;
    static Result<void> preserve_documents(Session &fresh, const Session &current,
                                           std::string_view mutated_member = {});
    static void adopt_refreshed_state(Session &current, Session &fresh) {
        for (auto &source : fresh.sources.members) {
            const auto previous = member(current, source->id);
            if (previous)
                source->reservation = (*previous)->reservation;
        }
        current.sources = std::move(fresh.sources);
        current.documents = std::move(fresh.documents);
        current.filesystem_index.reset();
        current.discovery = std::move(fresh.discovery);
        current.selected_formats = std::move(fresh.selected_formats);
        current.last_access = fresh.last_access;
        const std::scoped_lock lock{current.document_cursors.mutex};
        current.document_cursors.positions.clear();
        current.document_cursors.cursors.clear();
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
