#include "image_sessions_internal.hpp"

axk::app::Result<axk::app::ImageSessionRead>
axk::app::ImageSessionManager::begin_read(std::string_view image_id, std::string_view owner_id,
                                          std::uint64_t expected_revision) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    auto access = std::unique_lock{(*session)->access_mutex};
    if ((*session)->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if ((*session)->mutating)
        return std::unexpected(session_error("entry_in_use", "image session mutation is already active", true));
    if (!(*session)->media)
        return std::unexpected(session_error("image_media_unavailable", "image session media is unavailable", true));
    if (const auto unchanged = (*session)->verify_source_unchanged(); !unchanged)
        return std::unexpected(session_error("image_source_changed", "image source changed after it was opened", true));
    auto lease = std::make_shared<std::unique_lock<std::mutex>>(std::move(access));
    std::vector<const ObjectSnapshot *> catalog_objects;
    catalog_objects.reserve((*session)->snapshots_by_id.size());
    std::unordered_map<std::string, std::string> object_keys_by_id;
    object_keys_by_id.reserve((*session)->snapshots_by_id.size());
    for (const auto &[id, snapshot] : (*session)->snapshots_by_id) {
        catalog_objects.push_back(&snapshot);
        object_keys_by_id.emplace(id, snapshot.key);
    }
    std::unordered_map<std::string, ImageVolumeScopeIdentity> volume_scopes_by_id;
    for (const auto &item : (*session)->content) {
        if (item.kind == "volume" && item.partition_index && item.volume_directory_id) {
            volume_scopes_by_id.emplace(
                item.id, ImageVolumeScopeIdentity{*item.partition_index, *item.volume_directory_id, item.display_name});
        }
    }
    return ImageSessionRead{(*session)->image_id,
                            (*session)->revision,
                            (*session)->source,
                            (*session)->source_reader,
                            &*(*session)->media,
                            (*session)->target_snapshot_id,
                            std::move(catalog_objects),
                            (*session)->catalog_issues,
                            std::move(object_keys_by_id),
                            std::move(volume_scopes_by_id),
                            std::move(lease)};
}

axk::app::Result<axk::app::ImageSessionMutation>
axk::app::ImageSessionManager::begin_mutation(std::string_view image_id, std::string_view owner_id,
                                              std::uint64_t expected_revision) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    auto access = std::unique_lock{(*session)->access_mutex};
    if ((*session)->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if ((*session)->format != "sfs")
        return std::unexpected(session_error("image_mutation_unsupported", "only SFS image sessions can be altered"));
    const auto *container = (*session)->media ? std::get_if<Container>(&(*session)->media->storage()) : nullptr;
    if (container == nullptr || container->superblock().sector_size_bytes != 512U ||
        !std::ranges::all_of(container->partitions(),
                             [](const Partition &partition) { return partition.sectors_per_cluster == 2U; })) {
        return std::unexpected(session_error("image_mutation_unsupported",
                                             "image geometry is outside the supported 512-byte alteration profile"));
    }
    if ((*session)->mutating)
        return std::unexpected(session_error("entry_in_use", "image session mutation is already active", true));
    if (auto upgraded = (*session)->path_lease.try_upgrade(); !upgraded)
        return std::unexpected(upgraded.error());
    const FileRef source{(*session)->source.root_id, (*session)->source.relative_path};
    auto target = implementation_->sandbox.open_mutation(source);
    if (!target) {
        (*session)->path_lease.downgrade();
        return std::unexpected(target.error());
    }
    (*session)->mutating = true;
    (*session)->mutation_guard.emplace(std::move(access));
    return ImageSessionMutation{(*session)->image_id, (*session)->revision, std::move(source), std::move(*target)};
}

axk::app::Result<axk::app::PreparedImageSessionCommit>
axk::app::ImageSessionManager::prepare_mutation_commit(std::string_view image_id, std::string_view owner_id,
                                                       std::uint64_t expected_revision,
                                                       const CancellationToken &cancellation) {
    std::shared_ptr<Implementation::Session> session;
    {
        const std::scoped_lock lock{implementation_->mutex};
        const auto found = implementation_->sessions.find(std::string{image_id});
        if (found == implementation_->sessions.end() || found->second->owner_id != owner_id)
            return std::unexpected(session_error("image_not_found", "image session does not exist"));
        session = found->second;
    }
    if (!session->mutating || !session->mutation_guard || session->revision != expected_revision) {
        return std::unexpected(session_error("image_revision_stale", "image session mutation is not current", true));
    }

    ImageSessionManager refreshed{implementation_->sandbox, 1U, implementation_->maximum_page_size,
                                  implementation_->idle_retention, implementation_->clock};
    auto opened = refreshed.open(session->source, std::string{owner_id}, cancellation);
    if (!opened)
        return std::unexpected(opened.error());
    std::shared_ptr<Implementation::Session> fresh;
    {
        const std::scoped_lock lock{refreshed.implementation_->mutex};
        fresh = refreshed.implementation_->sessions.at(opened->image_id);
    }
    Implementation::preserve_object_ids(*session, *fresh);
    opened->image_id = session->image_id;
    opened->revision = expected_revision + 1U;
    PreparedImageSessionCommit prepared;
    prepared.image_id = session->image_id;
    prepared.expected_revision = expected_revision;
    prepared.summary = std::move(*opened);
    prepared.current_state = session;
    prepared.refreshed_state = std::move(fresh);
    return prepared;
}

axk::app::ImageSessionSummary
axk::app::ImageSessionManager::finalize_mutation_commit(PreparedImageSessionCommit prepared) noexcept {
    auto session = std::static_pointer_cast<Implementation::Session>(std::move(prepared.current_state));
    auto fresh = std::static_pointer_cast<Implementation::Session>(std::move(prepared.refreshed_state));
    Implementation::adopt_refreshed_state(*session, *fresh);
    implementation_->remove_auditions_for(session);
    ++session->revision;
    session->mutating = false;
    session->path_lease.downgrade();
    session->mutation_guard.reset();
    return std::move(prepared.summary);
}

axk::app::Result<axk::app::ImageSessionSummary>
axk::app::ImageSessionManager::commit_mutation(std::string_view image_id, std::string_view owner_id,
                                               std::uint64_t expected_revision, const CancellationToken &cancellation) {
    auto prepared = prepare_mutation_commit(image_id, owner_id, expected_revision, cancellation);
    if (!prepared)
        return std::unexpected(prepared.error());
    return finalize_mutation_commit(std::move(*prepared));
}

void axk::app::ImageSessionManager::abort_mutation(std::string_view image_id, std::string_view owner_id,
                                                   std::uint64_t expected_revision) noexcept {
    std::shared_ptr<Implementation::Session> session;
    {
        const std::scoped_lock lock{implementation_->mutex};
        const auto found = implementation_->sessions.find(std::string{image_id});
        if (found == implementation_->sessions.end() || found->second->owner_id != owner_id)
            return;
        session = found->second;
    }
    if (!session->mutating || !session->mutation_guard || session->revision != expected_revision)
        return;
    session->mutating = false;
    session->path_lease.downgrade();
    session->mutation_guard.reset();
}

axk::app::Result<void> axk::app::ImageSessionManager::close(std::string_view image_id, std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
    const auto found = implementation_->sessions.find(std::string{image_id});
    if (found == implementation_->sessions.end())
        return {};
    if (found->second->owner_id != owner_id)
        return std::unexpected(session_error("image_not_found", "image session does not exist"));
    const auto session = found->second;
    const std::unique_lock access{session->access_mutex, std::try_to_lock};
    if (!access)
        return std::unexpected(session_error("entry_in_use", "image session mutation is active", true));
    implementation_->sessions.erase(found);
    return {};
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageContentItem>>
axk::app::ImageSessionManager::content(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                                       std::optional<std::string_view> cursor,
                                       std::optional<std::string_view> parent_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto scope = parent_id.value_or("");
    const auto children = (*session)->content_children.find(std::string{scope});
    if (children == (*session)->content_children.end())
        return std::unexpected(session_error("content_not_found", "content parent does not exist"));
    return implementation_->page((*session)->content, (*session)->content_cursors, limit, cursor, scope,
                                 &children->second);
}

axk::app::Result<axk::app::ImageContentScope>
axk::app::ImageSessionManager::content_scope(std::string_view image_id, std::string_view owner_id,
                                             std::string_view content_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto item = std::ranges::find((*session)->content, content_id, &ImageContentItem::id);
    if (item == (*session)->content.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    const auto children = (*session)->content_children.find(std::string{content_id});
    if (children == (*session)->content_children.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    ImageContentScope result{*item, {}};
    result.children.reserve(children->second.size());
    for (const auto index : children->second)
        result.children.push_back((*session)->content.at(index));
    return result;
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageObjectItem>> axk::app::ImageSessionManager::objects(
    std::string_view image_id, std::string_view owner_id, std::size_t limit, std::optional<std::string_view> cursor,
    std::optional<std::string_view> object_type, std::optional<std::string_view> content_scope_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (!object_type && !content_scope_id)
        return implementation_->page((*session)->objects, (*session)->object_cursors, limit, cursor);

    const auto scope =
        "content:" + std::string{content_scope_id.value_or("")} + "\ntype:" + std::string{object_type.value_or("")};
    const std::vector<std::size_t> *indices = nullptr;
    static const std::vector<std::size_t> empty;
    if (content_scope_id) {
        const auto found = (*session)->object_indices_by_content_scope.find(std::string{*content_scope_id});
        if (found == (*session)->object_indices_by_content_scope.end())
            return std::unexpected(session_error("content_not_found", "content scope does not exist"));
        indices = &found->second;
    } else {
        const auto found = (*session)->object_indices_by_type.find(std::string{*object_type});
        indices = found == (*session)->object_indices_by_type.end() ? &empty : &found->second;
    }

    std::vector<std::size_t> filtered;
    if (content_scope_id && object_type) {
        filtered.reserve(indices->size());
        std::ranges::copy_if(*indices, std::back_inserter(filtered),
                             [&](const std::size_t index) { return (*session)->objects[index].type == *object_type; });
        indices = &filtered;
    }
    return implementation_->page((*session)->objects, (*session)->object_cursors, limit, cursor, scope, indices);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageRelationshipItem>>
axk::app::ImageSessionManager::relationships(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                                             std::optional<std::string_view> cursor, ImageRelationshipFilter filter) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (!filter.content_scope_id && !filter.source_object_id && !filter.target_object_id && !filter.relationship_type)
        return implementation_->page((*session)->relationships, (*session)->relationship_cursors, limit, cursor);

    const auto scope = "content:" + std::string{filter.content_scope_id.value_or("")} +
                       "\nsource:" + std::string{filter.source_object_id.value_or("")} +
                       "\ntarget:" + std::string{filter.target_object_id.value_or("")} +
                       "\ntype:" + std::string{filter.relationship_type.value_or("")};
    const std::vector<std::size_t> *content_indices = nullptr;
    if (filter.content_scope_id) {
        const auto found = (*session)->object_indices_by_content_scope.find(std::string{*filter.content_scope_id});
        if (found == (*session)->object_indices_by_content_scope.end())
            return std::unexpected(session_error("content_not_found", "content scope does not exist"));
        content_indices = &found->second;
    }

    std::vector<std::size_t> indices;
    indices.reserve((*session)->relationships.size());
    for (std::size_t index = 0U; index < (*session)->relationships.size(); ++index) {
        const auto &item = (*session)->relationships[index];
        if (filter.source_object_id && item.source_object_id != *filter.source_object_id)
            continue;
        if (filter.target_object_id && (!item.target_object_id || *item.target_object_id != *filter.target_object_id))
            continue;
        if (filter.relationship_type && item.type != *filter.relationship_type)
            continue;
        if (content_indices != nullptr) {
            const auto source = (*session)->object_indices_by_id.find(item.source_object_id);
            if (source == (*session)->object_indices_by_id.end() ||
                !std::ranges::binary_search(*content_indices, source->second)) {
                continue;
            }
        }
        indices.push_back(index);
    }
    return implementation_->page((*session)->relationships, (*session)->relationship_cursors, limit, cursor, scope,
                                 &indices);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageValidationItem>>
axk::app::ImageSessionManager::validation_issues(std::string_view image_id, std::string_view owner_id,
                                                 std::size_t limit, std::optional<std::string_view> cursor) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    return implementation_->page((*session)->validation, (*session)->validation_cursors, limit, cursor);
}

void axk::app::ImageSessionManager::cleanup() {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
}
