#include "image_sessions_internal.hpp"

#include <set>

#include "axklib/native/registry.hpp"
#include "formats/a_series/admission.hpp"
#include "formats/tx16w/admission.hpp"

namespace axk::app {

Result<void> ImageSessionManager::Implementation::discover(Session &session, const CancellationToken &cancellation,
                                                           ProgressSink *progress, diagnostic_sink *diagnostics) {
    auto a_series = formats::a_series::admit_documents(sandbox, session.sources, cancellation, progress, diagnostics);
    if (!a_series)
        return std::unexpected(a_series.error());
    native::FormatRegistry registry;
    native::FormatProvider a_provider;
    a_provider.descriptor = {"yamaha.a-series", "Yamaha A-series", {"a-series"}};
    a_provider.recognize = [&a_series](const native::SourceSet &sources,
                                       const CancellationToken &) -> axk::Result<native::Recognition> {
        native::Recognition recognition{};
        recognition.format_id = "yamaha.a-series";
        if (a_series->empty())
            return recognition;
        recognition.state = native::RecognitionState::supported;
        for (const auto &document : *a_series) {
            const auto claim_count = recognition.claims.size();
            for (const auto &object : document.native->native_document->catalog().objects)
                recognition.claims.insert(recognition.claims.end(), object.locations.begin(), object.locations.end());
            if (recognition.claims.size() == claim_count)
                for (const auto &member_id : document.member_ids) {
                    auto location = sources.locate(member_id, {}, {});
                    if (location)
                        recognition.claims.push_back(*location);
                }
        }
        return recognition;
    };
    a_provider.open =
        [&a_series](std::shared_ptr<const native::SourceSet>,
                    const CancellationToken &) -> axk::Result<std::vector<std::shared_ptr<const native::Document>>> {
        std::vector<std::shared_ptr<const native::Document>> documents;
        for (const auto &item : *a_series)
            documents.push_back(item.native->native_document);
        return documents;
    };
    if (auto added = registry.add(std::move(a_provider)); !added)
        return std::unexpected(core_error(added.error(), session.sources.members.front()->reference));
    if (auto added = registry.add(axk::tx16w::provider()); !added)
        return std::unexpected(core_error(added.error(), session.sources.members.front()->reference));
    auto opened = registry.open(session.sources.native_sources, session.selected_formats, cancellation);
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
        auto document_state = std::make_shared<Document>();
        if (native_document->summary().format_id == "yamaha.a-series") {
            const auto found =
                std::ranges::find(*a_series, native_document->summary().id, &NativeDocumentAdmission::id);
            if (found == a_series->end())
                return std::unexpected(session_error("document_invalid", "Document admission identity is missing"));
            static_cast<NativeDocumentAdmission &>(*document_state) = std::move(*found);
        } else {
            const auto tx16w = std::dynamic_pointer_cast<const axk::tx16w::Document>(native_document);
            if (!tx16w)
                return std::unexpected(session_error("document_invalid", "No application adapter for native document"));
            static_cast<NativeDocumentAdmission &>(*document_state) =
                formats::tx16w::admit_document(tx16w, session.sources);
        }
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
