#include "image_sessions_internal.hpp"

#include <set>
#include <utility>

#include "session_formats.hpp"

namespace axk::app {

Result<void> ImageSessionManager::Implementation::discover(Session &session, const CancellationToken &cancellation,
                                                           ProgressSink *progress, diagnostic_sink *diagnostics,
                                                           const Session *previous,
                                                           std::string_view extended_document) {
    SessionFormats formats;
    // New companions belong only to the selected document. Do not reopen them in the
    // scope of another previously opened Setup that happens to request the same Wave.
    if (previous)
        for (const auto &document : previous->documents) {
            if (!document->companions.can_attach)
                continue;
            auto members = document->member_ids;
            if (document->id == extended_document)
                for (auto i = previous->sources.members.size(); i < session.sources.members.size(); ++i)
                    members.push_back(session.sources.members[i]->id);
            formats.retain_member_group(document->format, std::move(members));
        }
    if (auto configured = configure_formats(formats, {session.sources, cancellation, progress, diagnostics});
        !configured)
        return std::unexpected(configured.error());
    auto opened = formats.registry().open(session.sources.native_sources, session.selected_formats, cancellation);
    if (!opened)
        return std::unexpected(core_error(opened.error(), session.sources.members.front()->reference));
    session.discovery.selection_required = opened->selection_required;
    for (const auto &candidate : opened->candidates) {
        if (candidate.state == native::RecognitionState::no_match)
            continue;
        ImageDiscoveryCandidate value{candidate.format_id,
                                      candidate.state == native::RecognitionState::supported        ? "SUPPORTED"
                                      : candidate.state == native::RecognitionState::incomplete     ? "INCOMPLETE"
                                      : candidate.state == native::RecognitionState::limit_exceeded ? "LIMIT_EXCEEDED"
                                                                                                    : "INVALID",
                                      {},
                                      {}};
        std::set<std::string> members;
        for (const auto &claim : candidate.claims)
            members.insert(claim.member_id);
        value.source_member_ids.assign(members.begin(), members.end());
        for (const auto &diagnostic : candidate.diagnostics)
            value.diagnostics.push_back(diagnostic.message);
        session.discovery.candidates.push_back(std::move(value));
    }
    for (const auto &native_document : opened->documents) {
        auto admission = formats.bind(native_document);
        if (!admission)
            return std::unexpected(admission.error());
        auto document_state = std::make_shared<Document>();
        static_cast<NativeDocumentAdmission &>(*document_state) = std::move(*admission);
        session.documents.push_back(std::move(document_state));
    }
    return {};
}

Result<void> ImageSessionManager::Implementation::preserve_documents(Session &fresh, const Session &current,
                                                                     std::string_view mutated_member) {
    const auto untouched = [mutated_member](const Document &document) {
        std::set<std::string> members;
        for (const auto &member : document.member_ids)
            if (member != mutated_member)
                members.insert(member);
        return members;
    };
    if (!mutated_member.empty())
        for (const auto &previous : current.documents)
            if (!document(fresh, previous->id) && !untouched(*previous).empty())
                return std::unexpected(
                    session_error("document_membership_changed", "An unrelated native document disappeared"));
    for (const auto &document_state : fresh.documents) {
        const auto previous = document(current, document_state->id);
        if (previous) {
            if (untouched(*document_state) != untouched(**previous))
                return std::unexpected(
                    session_error("document_membership_changed", "Native document membership changed"));
            if (auto preserved = document_state->native->preserve_identity(*(*previous)->native); !preserved)
                return preserved;
        } else if (!mutated_member.empty() && !untouched(*document_state).empty())
            return std::unexpected(
                session_error("document_membership_changed", "An unrelated native document appeared"));
    }
    return {};
}

} // namespace axk::app
