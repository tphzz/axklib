#include "image_sessions_internal.hpp"

namespace axk::app {
Result<ImageSessionManager::Implementation::AuditionAccess>
ImageSessionManager::Implementation::access_audition(std::string_view audition_id, std::string_view owner_id) {
    AuditionAccess result;
    std::string document_id;
    {
        const std::scoped_lock lock{mutex};
        cleanup_locked();
        const auto found = audition_sessions.find(std::string{audition_id});
        if (found != audition_sessions.end()) {
            result.session = found->second.session.lock();
            document_id = found->second.document_id;
            if (!result.session)
                audition_sessions.erase(found);
        }
    }
    if (!result.session || result.session->owner_id != owner_id)
        return std::unexpected(session_error("audition_not_found", "audition does not exist"));
    {
        const ReadDiagnosticTimer wait{ReadMetric::session_wait};
        result.lock = std::unique_lock{result.session->access_mutex};
    }
    if (result.session->invalidated || result.session->mutating)
        return std::unexpected(session_error("image_session_invalidated", "image session requires recovery"));
    auto selected = document(*result.session, document_id);
    if (!selected)
        return std::unexpected(selected.error());
    result.document = *selected;
    const auto found = result.document->native->auditions.find(std::string{audition_id});
    if (found == result.document->native->auditions.end() || !result.document->native->audio() ||
        found->second.descriptor.revision != result.session->revision ||
        found->second.last_access + std::chrono::minutes{10} <= clock())
        return std::unexpected(session_error("audition_not_found", "Audition expired or its document changed"));
    result.entry = &found->second;
    return result;
}

Result<ImageAudition> ImageSessionManager::audition_metadata(std::string_view audition_id, std::string_view owner_id) {
    auto access = implementation_->access_audition(audition_id, owner_id);
    if (!access)
        return std::unexpected(access.error());
    return access->entry->descriptor;
}
} // namespace axk::app
