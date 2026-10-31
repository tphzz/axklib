#include "image_sessions_internal.hpp"

namespace axk::app {

Result<NativeDocumentSnapshot> ImageSessionManager::native_snapshot(const ImageDocumentRef &target,
                                                                    std::string_view owner_id,
                                                                    std::uint64_t expected_revision,
                                                                    const CancellationToken &cancellation) {
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(Error{"operation_cancelled", active.error().message});
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    if ((*session)->invalidated || (*session)->mutating || (*session)->revision != expected_revision)
        return std::unexpected(
            session_error("image_revision_stale", "Source workspace changed; prepare a new plan", true));
    const auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    if (const auto verified = (*session)->sources.native_sources->verify(cancellation); !verified)
        return std::unexpected(core_error(verified.error(), (*session)->sources.members.front()->reference));
    const auto document = (*selected)->native->native_document;
    if (!document)
        return std::unexpected(session_error("native_format_unsupported", "Source has no native conversion document"));
    return NativeDocumentSnapshot{target, expected_revision, document,
                                  [this, target, owner = std::string{owner_id},
                                   expected_revision](const CancellationToken &token) -> Result<void> {
                                      const auto current = native_snapshot(target, owner, expected_revision, token);
                                      if (!current)
                                          return std::unexpected(current.error());
                                      return {};
                                  }};
}

} // namespace axk::app
