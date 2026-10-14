#include "axklib/application/formats/a_series/session_read.hpp"

#include <utility>

#include "formats/a_series/session_state.hpp"
#include "image_sessions_internal.hpp"

namespace axk::app::formats::a_series {

Result<SessionRead> SessionReader::acquire(ImageSessionManager &sessions, const ImageDocumentRef &target,
                                           std::string_view owner_id, std::uint64_t expected_revision) {
    const auto session = sessions.implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    auto source = sessions.begin_read(target, owner_id, expected_revision);
    if (!source)
        return std::unexpected(source.error());
    auto selected = ImageSessionManager::Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    const auto *native = dynamic_cast<const SessionState *>((*selected)->native.get());
    if (native == nullptr)
        return std::unexpected(
            session_error("native_format_unsupported", "This operation requires an A-series document"));
    SessionRead read;
    static_cast<ImageSessionRead &>(read) = std::move(*source);
    read.catalog_issues = native->catalog_issues;
    read.catalog_objects.reserve(native->snapshots_by_id.size());
    read.object_keys_by_id.reserve(native->snapshots_by_id.size());
    for (const auto &[id, snapshot] : native->snapshots_by_id) {
        read.catalog_objects.push_back(snapshot);
        read.object_keys_by_id.emplace(id, snapshot->key);
    }
    for (const auto &item : native->content) {
        if (item.kind == "volume" && item.partition_index && item.volume_directory_id)
            read.volume_scopes_by_id.emplace(
                item.id, VolumeScopeIdentity{*item.partition_index, *item.volume_directory_id, item.display_name});
    }
    return read;
}

} // namespace axk::app::formats::a_series
