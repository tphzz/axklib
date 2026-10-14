#include "content_digest.hpp"
#include "image_sessions_internal.hpp"

namespace axk::app {

Result<ImageSessionRead> ImageSessionManager::begin_read(const ImageDocumentRef &target, std::string_view owner_id,
                                                         std::uint64_t expected_revision) {
    return begin_read_access(target.image_id, owner_id, expected_revision, {}, target.document_id);
}

Result<ImageSessionRead> ImageSessionManager::begin_source_read(const ImageSourceTarget &target,
                                                                std::string_view owner_id,
                                                                std::uint64_t expected_revision) {
    return begin_read_access(target.image_id, owner_id, expected_revision, target.member_id, {});
}

Result<ImageSessionRead> ImageSessionManager::begin_read_access(std::string_view image_id, std::string_view owner_id,
                                                                std::uint64_t expected_revision,
                                                                std::string_view member_id,
                                                                std::string_view document_id) {
    auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    auto access = std::unique_lock{(*session)->access_mutex};
    if ((*session)->invalidated)
        return std::unexpected(
            session_error("image_session_invalidated", "image session requires recovery and reopening"));
    if ((*session)->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if ((*session)->mutating)
        return std::unexpected(session_error("entry_in_use", "image session mutation is already active", true));
    std::shared_ptr<Implementation::Document> document_state;
    if (!document_id.empty()) {
        auto selected = Implementation::document(**session, document_id);
        if (!selected)
            return std::unexpected(selected.error());
        document_state = *selected;
        if (!document_state->primary)
            return std::unexpected(
                session_error("native_format_unsupported", "Document has no single media projection"));
        member_id = document_state->primary->id;
    }
    auto selected_member = Implementation::member(**session, member_id);
    if (!selected_member)
        return std::unexpected(selected_member.error());
    const auto member = *selected_member;
    if (auto verified = (*session)->sources.native_sources->verify(); !verified)
        return std::unexpected(core_error(verified.error(), member->reference));
    struct Lease {
        std::shared_ptr<Implementation::Session> owner;
        std::unique_lock<std::mutex> lock;
    };
    auto lease = std::make_shared<Lease>(Lease{*session, std::move(access)});
    const auto reader = document_state && document_state->snapshot ? document_state->snapshot : member->reader;
    const auto media = document_state ? document_state->media : member->media;
    auto verify = [sources = (*session)->sources.native_sources, reference = member->reference]() -> Result<void> {
        auto checked = sources->verify();
        if (!checked)
            return std::unexpected(core_error(checked.error(), reference));
        return {};
    };
    auto fingerprint = [member, reader, verify, lease](const CancellationToken &cancellation) -> Result<std::string> {
        static_cast<void>(lease);
        const std::scoped_lock cache{member->fingerprint_mutex};
        if (auto active = cancellation.check(); !active)
            return std::unexpected(core_error(active.error(), member->reference));
        if (auto checked = verify(); !checked)
            return std::unexpected(checked.error());
        if (member->reference.kind == ImageSourceKind::device)
            return "device:" + member->revision;
        if (!reader)
            return std::unexpected(session_error("image_media_unavailable", "Source has no byte stream"));
        if (!member->content_fingerprint) {
            auto digest = detail::reader_sha256(*reader, cancellation);
            if (!digest)
                return std::unexpected(digest.error());
            if (auto checked = verify(); !checked)
                return std::unexpected(checked.error());
            member->content_fingerprint = std::move(*digest);
        }
        return *member->content_fingerprint;
    };
    return ImageSessionRead{.image_id = (*session)->image_id,
                            .revision = (*session)->revision,
                            .source = member->reference,
                            .reader = reader,
                            .media = media.get(),
                            .content_fingerprint = std::move(fingerprint),
                            .lease = std::move(lease),
                            .verify_source_unchanged = std::move(verify),
                            .source_member_id = member->id,
                            .document_id = std::string{document_id}};
}

} // namespace axk::app
