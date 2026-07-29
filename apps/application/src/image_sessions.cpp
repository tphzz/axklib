#include "image_sessions_internal.hpp"

axk::app::ImageSessionManager::Implementation::PendingAdmission::PendingAdmission(Implementation &implementation)
    : implementation_(&implementation) {}

axk::app::ImageSessionManager::Implementation::PendingAdmission::~PendingAdmission() {
    if (!active_)
        return;
    const std::scoped_lock lock{implementation_->mutex};
    --implementation_->pending_sessions;
}

bool axk::app::ImageSessionManager::Implementation::PendingAdmission::promote(const std::shared_ptr<Session> &session) {
    const std::scoped_lock lock{implementation_->mutex};
    if (implementation_->sessions.contains(session->image_id))
        return false;
    implementation_->sessions.emplace(session->image_id, session);
    --implementation_->pending_sessions;
    active_ = false;
    return true;
}

axk::app::ImageSessionManager::ImageSessionManager(const Sandbox &sandbox, std::size_t maximum_sessions,
                                                   std::size_t maximum_page_size, std::chrono::seconds idle_retention,
                                                   Clock clock, PathReservationCoordinator *path_reservations,
                                                   std::uint64_t maximum_audition_bundle_bytes,
                                                   std::size_t maximum_audition_clips)
    : implementation_(std::make_unique<Implementation>(sandbox, maximum_sessions, maximum_page_size, idle_retention,
                                                       std::move(clock), path_reservations,
                                                       maximum_audition_bundle_bytes, maximum_audition_clips)) {}

axk::app::ImageSessionManager::~ImageSessionManager() = default;
axk::app::ImageSessionManager::ImageSessionManager(ImageSessionManager &&) noexcept = default;
axk::app::ImageSessionManager &axk::app::ImageSessionManager::operator=(ImageSessionManager &&) noexcept = default;
