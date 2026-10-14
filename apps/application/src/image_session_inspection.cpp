#include "image_sessions_internal.hpp"

namespace axk::app {

Result<ImageDocumentSummary> ImageSessionManager::Implementation::summarize(const Session &session,
                                                                            const Document &document_state) const {
    const auto &native_summary = document_state.native->native_document->summary();
    ImageDocumentSummary summary;
    summary.id = document_state.id;
    summary.format_id = std::string{document_state.native->format_id()};
    summary.profile = std::string{document_state.native->profile()};
    summary.name = native_summary.name;
    summary.completeness = native_summary.completeness == native::Completeness::complete     ? "COMPLETE"
                           : native_summary.completeness == native::Completeness::incomplete ? "INCOMPLETE"
                                                                                             : "INVALID";
    summary.format = document_state.format;
    summary.floppy_set = document_state.floppy_set;
    summary.companion_sources = document_state.companion_sources;
    summary.companions = document_state.companions;
    summary.source_member_ids = document_state.member_ids;
    summary.root_count = document_state.native->root_count;
    summary.object_count = document_state.native->objects.size();
    summary.relationship_count = document_state.native->relationships.size();
    for (const auto &issue : document_state.native->validation) {
        if (issue.severity == "error")
            ++summary.validation.error_count;
        else if (issue.severity == "warning")
            ++summary.validation.warning_count;
        else
            ++summary.validation.info_count;
    }
    bool source_writable{};
    bool device_writable{};
    if (document_state.primary && document_state.member_ids.size() == 1U) {
        const auto &source = document_state.primary->reference;
        if (source.kind == ImageSourceKind::device && devices) {
            auto status = devices->status(source.device_session_id, session.owner_id);
            device_writable = status && status->writes_authorized && !status->invalidated;
        } else {
            auto metadata = sandbox.metadata(source.root_id, source.relative_path);
            source_writable = metadata && metadata->writable;
        }
    }
    summary.available_operations = document_state.native->available_operations(source_writable, device_writable);
    return summary;
}

Result<ImageSessionSummary> ImageSessionManager::inspect(std::string_view image_id, std::string_view owner_id) {
    auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    ImageSessionSummary summary;
    summary.image_id = (*session)->image_id;
    summary.revision = (*session)->revision;
    summary.discovery = (*session)->discovery;
    for (const auto &member : (*session)->sources.members) {
        ImageSourceSummary source{member->id, member->reference, member->format, {}};
        for (const auto &root : member->roots)
            source.filesystem_root_ids.push_back(member->id + ":" + root.id);
        if (member->media)
            for (const auto &issue : member->media->validation_issues())
                source.diagnostics.push_back({issue.code,
                                              issue.severity == ValidationSeverity::error     ? "error"
                                              : issue.severity == ValidationSeverity::warning ? "warning"
                                                                                              : "info",
                                              issue.message, issue.sampler_path});
        summary.sources.push_back(std::move(source));
    }
    for (const auto &document_state : (*session)->documents) {
        auto value = implementation_->summarize(**session, *document_state);
        if (!value)
            return std::unexpected(value.error());
        summary.documents.push_back(std::move(*value));
    }
    return summary;
}

Result<ImageDocumentSummary> ImageSessionManager::document(const ImageDocumentRef &target, std::string_view owner_id) {
    auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    return implementation_->summarize(**session, **selected);
}

Result<ImagePage<ImageDocumentSummary>> ImageSessionManager::documents(std::string_view image_id,
                                                                       std::string_view owner_id, std::size_t limit,
                                                                       std::optional<std::string_view> cursor) {
    auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    std::vector<ImageDocumentSummary> values;
    for (const auto &document_state : (*session)->documents) {
        auto value = implementation_->summarize(**session, *document_state);
        if (!value)
            return std::unexpected(value.error());
        values.push_back(std::move(*value));
    }
    return implementation_->page(values, (*session)->document_cursors, limit, cursor);
}

} // namespace axk::app
