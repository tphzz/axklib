#include "image_sessions_internal.hpp"

#include <algorithm>
#include <unordered_map>

namespace axk::app {

Result<NativeDocumentSnapshot> ImageSessionManager::native_snapshot(const ImageDocumentRef &target,
                                                                    std::string_view owner_id,
                                                                    std::uint64_t expected_revision,
                                                                    const CancellationToken &cancellation) {
    const auto with_current = [this, target, owner = std::string{owner_id},
                               expected_revision](const CancellationToken &token, const auto &visit) -> Result<void> {
        if (const auto active = token.check(); !active)
            return std::unexpected(Error{"operation_cancelled", active.error().message});
        const auto session = implementation_->owned(target.image_id, owner);
        if (!session)
            return std::unexpected(session.error());
        const std::scoped_lock access{(*session)->access_mutex};
        if ((*session)->invalidated || (*session)->mutating || (*session)->revision != expected_revision)
            return std::unexpected(
                session_error("image_revision_stale", "Source workspace changed; prepare a new plan", true));
        const auto selected = Implementation::document(**session, target.document_id);
        if (!selected)
            return std::unexpected(selected.error());
        if (const auto verified = (*session)->sources.native_sources->verify(token); !verified)
            return std::unexpected(core_error(verified.error(), (*session)->sources.members.front()->reference));
        if (!(*selected)->native->native_document)
            return std::unexpected(
                session_error("native_format_unsupported", "Source has no native conversion document"));
        visit(*(*selected)->native);
        return {};
    };
    NativeDocumentSnapshot snapshot{target, expected_revision, {}, [with_current](const CancellationToken &token) {
                                        return with_current(token, [](const auto &) {});
                                    }};
    const auto checked = with_current(cancellation, [&](const auto &native) {
        snapshot.document = native.native_document;
        const auto &ids = native.native_object_ids;
        if (std::ranges::any_of(ids, [](const auto &item) { return item.first != item.second; })) {
            auto keys = std::make_shared<std::unordered_map<std::string, std::string>>();
            keys->reserve(ids.size());
            for (const auto &[key, id] : ids)
                keys->emplace(id, key);
            snapshot.native_keys_by_id = std::move(keys);
        }
    });
    if (!checked)
        return std::unexpected(checked.error());
    return snapshot;
}

} // namespace axk::app
