#include "image_sessions_internal.hpp"

#include "axklib/diagnostics.hpp"
#include "formats/a_series/directory.hpp"
#include "formats/a_series/floppy_set.hpp"

namespace axk::app {

Result<ImageSessionSummary> ImageSessionManager::open(const ImageOpenSources &request, std::string owner_id,
                                                      const CancellationToken &cancellation, ProgressSink *progress,
                                                      diagnostic_sink *diagnostics) {
    if (owner_id.empty())
        return std::unexpected(session_error("invalid_owner", "image session owner is required"));
    auto admission = implementation_->reserve_session();
    if (!admission)
        return std::unexpected(admission.error());
    DiagnosticTimer timer{diagnostics, diagnostic_source::filesystem, "image_open_phase", "source-admission"};
    auto sources =
        admit_image_sources(implementation_->sandbox, implementation_->path_reservations, implementation_->devices,
                            request.sources, owner_id, cancellation, progress, diagnostics);
    if (!sources)
        return std::unexpected(sources.error());
    timer.finish();
    auto session = std::make_shared<Implementation::Session>();
    session->owner_id = std::move(owner_id);
    session->sources = std::move(*sources);
    session->selected_formats = request.selected_formats;
    session->last_access = implementation_->clock();
    if (auto discovered = implementation_->discover(*session, cancellation, progress, diagnostics); !discovered)
        return std::unexpected(discovered.error());
    for (const auto &member : session->sources.members)
        if (member->seal)
            if (auto sealed = member->seal(cancellation, progress); !sealed)
                return std::unexpected(sealed.error());
    if (auto verified = session->sources.native_sources->verify(cancellation); !verified)
        return std::unexpected(core_error(verified.error(), request.sources.front()));
    do {
        auto image_id = random_identifier("image-");
        if (!image_id)
            return std::unexpected(image_id.error());
        session->image_id = std::move(*image_id);
        if ((*admission)->promote(session))
            break;
    } while (true);
    if (progress)
        progress->report({ProgressPhase::publishing, 1U, 1U, "Image session ready", std::nullopt});
    return inspect(session->image_id, session->owner_id);
}

Result<ImageSessionSummary> ImageSessionManager::select_documents(std::string_view image_id, std::string_view owner_id,
                                                                  std::uint64_t expected_revision,
                                                                  const std::vector<std::string> &selected_formats,
                                                                  const CancellationToken &cancellation) {
    auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    {
        const std::scoped_lock access{(*session)->access_mutex};
        if ((*session)->revision != expected_revision || (*session)->mutating)
            return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
        if (auto verified = (*session)->sources.native_sources->verify(cancellation); !verified)
            return std::unexpected(core_error(verified.error(), (*session)->sources.members.front()->reference));
        Implementation::Session fresh;
        fresh.sources = (*session)->sources;
        fresh.selected_formats = selected_formats;
        if (auto discovered = implementation_->discover(fresh, cancellation); !discovered)
            return std::unexpected(discovered.error());
        if (auto preserved = Implementation::preserve_documents(fresh, **session); !preserved)
            return std::unexpected(preserved.error());
        if (auto verified = fresh.sources.native_sources->verify(cancellation); !verified)
            return std::unexpected(core_error(verified.error(), fresh.sources.members.front()->reference));
        fresh.last_access = implementation_->clock();
        Implementation::adopt_refreshed_state(**session, fresh);
        ++(*session)->revision;
    }
    implementation_->remove_auditions_for(*session);
    return inspect(image_id, owner_id);
}

Result<ImageSessionSummary> ImageSessionManager::attach_companions(const ImageDocumentRef &target,
                                                                   std::string_view owner_id,
                                                                   std::uint64_t expected_revision,
                                                                   const CompanionSelection &selection,
                                                                   const CancellationToken &cancellation) {
    auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    {
        const std::scoped_lock access{(*session)->access_mutex};
        if ((*session)->revision != expected_revision || (*session)->mutating)
            return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
        auto document = Implementation::document(**session, target.document_id);
        if (!document)
            return std::unexpected(document.error());
        auto candidates = selection.sources;
        if (selection.kind == CompanionSelectionKind::immediate_siblings) {
            if (!selection.sources.empty() || !(*document)->primary || !(*document)->floppy_set)
                return std::unexpected(session_error("invalid_companion_sources", "Select explicit companion sources"));
            const auto &source = (*document)->primary->reference;
            auto siblings =
                source.kind == ImageSourceKind::directory
                    ? formats::a_series::sibling_directory_sources(
                          implementation_->sandbox, source, (*document)->floppy_set->set_label,
                          (*document)->floppy_set->status != ImageFloppySetStatus::recovery, cancellation)
                    : formats::a_series::immediate_sibling_floppy_sources(
                          implementation_->sandbox, source, (*document)->floppy_set->set_label, cancellation);
            if (!siblings)
                return std::unexpected(siblings.error());
            candidates = std::move(*siblings);
        }
        if (candidates.empty() || candidates.size() > 31U)
            return std::unexpected(session_error("invalid_companion_sources", "Select between one and 31 companions"));
        std::vector<ImageSourceRef> references;
        for (const auto &member : (*session)->sources.members)
            references.push_back(member->reference);
        const auto original_count = references.size();
        for (const auto &candidate : candidates)
            if (std::ranges::find(references, candidate) == references.end())
                references.push_back(candidate);
        if (references.size() == original_count)
            return std::unexpected(session_error("companion_segment_not_found", "No new companion source selected"));
        auto sources = admit_image_sources(implementation_->sandbox, implementation_->path_reservations,
                                           implementation_->devices, references, owner_id, cancellation);
        if (!sources)
            return std::unexpected(sources.error());
        Implementation::Session fresh;
        fresh.sources = std::move(*sources);
        fresh.selected_formats = (*session)->selected_formats;
        if (auto discovered = implementation_->discover(fresh, cancellation); !discovered)
            return std::unexpected(discovered.error());
        auto updated = Implementation::document(fresh, target.document_id);
        if (!updated || (*updated)->member_ids == (*document)->member_ids)
            return std::unexpected(
                session_error("companion_segment_not_found", "Selected sources do not extend this document"));
        for (const auto &item : fresh.documents) {
            auto previous = Implementation::document(**session, item->id);
            if (previous)
                if (auto preserved = item->native->preserve_identity(*(*previous)->native); !preserved)
                    return std::unexpected(preserved.error());
        }
        if (auto verified = (*session)->sources.native_sources->verify(cancellation); !verified)
            return std::unexpected(core_error(verified.error(), references.front()));
        if (auto verified = fresh.sources.native_sources->verify(cancellation); !verified)
            return std::unexpected(core_error(verified.error(), references.front()));
        fresh.last_access = implementation_->clock();
        Implementation::adopt_refreshed_state(**session, fresh);
        ++(*session)->revision;
    }
    implementation_->remove_auditions_for(*session);
    return inspect(target.image_id, owner_id);
}

} // namespace axk::app
