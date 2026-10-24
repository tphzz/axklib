#include "image_sessions_internal.hpp"
#include "read_diagnostics.hpp"

axk::app::Result<nlohmann::ordered_json> axk::app::ImageSessionManager::object_detail(const ImageDocumentRef &target,
                                                                                      std::string_view owner_id,
                                                                                      std::string_view object_id,
                                                                                      diagnostic_sink *diagnostics) {
    ReadDiagnosticScope trace{diagnostics, "image_object_read"};
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    trace.phase("lock-wait");
    const std::scoped_lock access{(*session)->access_mutex};
    trace.phase("metadata");
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    const auto summary = implementation_->summarize(**session, **selected);
    if (!summary)
        return std::unexpected(summary.error());
    trace.phase("source-verify");
    if (const auto unchanged = (*session)->sources.native_sources->verify(); !unchanged)
        return std::unexpected(session_error("image_source_changed", unchanged.error().message));
    trace.phase("object-detail");
    auto result = (*selected)->native->object_detail(
        {target.image_id, target.document_id, (*session)->revision, summary->format, summary->available_operations},
        object_id);
    if (result)
        trace.complete();
    return result;
}
