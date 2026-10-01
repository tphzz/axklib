#include "image_sessions_internal.hpp"

axk::app::Result<axk::app::ImageFilesystemPage>
axk::app::ImageSessionManager::filesystem(std::string_view image_id, std::string_view owner_id,
                                          std::uint64_t expected_revision, const ImageFilesystemQuery &query) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto &state = *session;
    const std::scoped_lock lock{state->access_mutex};
    if (state->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if (state->mutating || !state->media)
        return std::unexpected(session_error("entry_in_use", "image session is not readable", true));
    if (const auto unchanged = state->verify_source_unchanged(); !unchanged)
        return std::unexpected(unchanged.error());
    if (query.limit == 0U || query.limit > implementation_->maximum_page_size || query.query.size() > 256U)
        return std::unexpected(
            session_error("invalid_request", "Filesystem page or query is outside the supported bounds"));
    for (const auto *identity :
         {&query.parent_id, &query.root_id, &query.entry_id, &query.object_id, &query.content_scope_id})
        if (identity->size() > 512U)
            return std::unexpected(session_error("invalid_request", "Filesystem identity exceeds the supported limit"));
    const auto selectors =
        static_cast<unsigned>(!query.parent_id.empty()) + static_cast<unsigned>(!query.entry_id.empty()) +
        static_cast<unsigned>(!query.object_id.empty()) + static_cast<unsigned>(!query.content_scope_id.empty()) +
        static_cast<unsigned>(!query.root_id.empty());
    if (selectors > 1U || (!query.query.empty() && query.root_id.empty()))
        return std::unexpected(session_error("invalid_request", "Choose one filesystem scope; search requires a root"));
    if (!state->filesystem_index) {
        auto index = detail::build_image_filesystem(*state->media, state->source_reader, state->snapshots_by_id,
                                                    state->descriptors_by_id, state->content);
        if (!index)
            return std::unexpected(index.error());
        state->filesystem_index = std::move(*index);
    }
    const auto &index = *state->filesystem_index;
    for (const auto *identity : {&query.parent_id, &query.root_id, &query.entry_id}) {
        if (identity->empty())
            continue;
        const auto entry = std::ranges::find(index.entries, *identity, &ImageFilesystemEntry::id);
        if (entry == index.entries.end() || (identity == &query.root_id && entry->parent_id) ||
            (identity == &query.parent_id && entry->kind == "file"))
            return std::unexpected(session_error("filesystem_entry_not_found", "Filesystem scope does not exist"));
    }
    ImageFilesystemPage page{state->revision,        index.available, index.filesystem_name, index.device_view, {}, 0U,
                             index.root_capabilities};
    const auto source = implementation_->sandbox.metadata(state->source.root_id, state->source.relative_path);
    if (!implementation_->path_reservations || !source || !source->writable ||
        state->source.kind != ImageSourceKind::file)
        for (auto &capability : page.root_capabilities) {
            capability.create_directory = capability.put_file = capability.delete_entry = capability.rename_entry =
                capability.move_entry = false;
            capability.supported_imports.clear();
        }
    const auto search = fold_ascii(query.query);
    for (const auto &entry : index.entries) {
        const auto matches = [&]() {
            if (!query.entry_id.empty())
                return entry.id == query.entry_id;
            if (!query.object_id.empty())
                return entry.object_id == query.object_id;
            if (!query.content_scope_id.empty())
                return entry.content_scope_id == query.content_scope_id;
            if (!query.root_id.empty())
                return entry.root_id == query.root_id && entry.parent_id &&
                       fold_ascii(entry.name).find(search) != std::string::npos;
            if (!query.parent_id.empty())
                return entry.parent_id == query.parent_id;
            return !entry.parent_id;
        };
        if (!matches())
            continue;
        if (page.total_count >= query.offset && page.items.size() < query.limit)
            page.items.push_back(entry);
        ++page.total_count;
    }
    return page;
}
