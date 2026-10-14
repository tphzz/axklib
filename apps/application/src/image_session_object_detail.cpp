#include "image_sessions_internal.hpp"

axk::app::Result<nlohmann::ordered_json> axk::app::ImageSessionManager::object_detail(const ImageDocumentRef &target,
                                                                                      std::string_view owner_id,
                                                                                      std::string_view object_id) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    const auto summary = implementation_->summarize(**session, **selected);
    if (!summary)
        return std::unexpected(summary.error());
    if (const auto unchanged = (*session)->sources.native_sources->verify(); !unchanged)
        return std::unexpected(session_error("image_source_changed", unchanged.error().message));
    return (*selected)->native->object_detail(
        {target.image_id, target.document_id, (*session)->revision, summary->format, summary->available_operations},
        object_id);
}
