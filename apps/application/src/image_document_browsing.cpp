#include "image_sessions_internal.hpp"

axk::app::Result<axk::app::ImagePage<axk::app::ImageContentItem>>
axk::app::ImageSessionManager::content(const ImageDocumentRef &target, std::string_view owner_id, std::size_t limit,
                                       std::optional<std::string_view> cursor,
                                       std::optional<std::string_view> parent_id) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    const auto scope = parent_id.value_or("");
    const auto children = (*selected)->native->content_children.find(std::string{scope});
    if (children == (*selected)->native->content_children.end())
        return std::unexpected(session_error("content_not_found", "content parent does not exist"));
    return implementation_->page((*selected)->native->content, (*selected)->content_cursors, limit, cursor, scope,
                                 &children->second);
}

axk::app::Result<axk::app::ImageContentScope>
axk::app::ImageSessionManager::content_scope(const ImageDocumentRef &target, std::string_view owner_id,
                                             std::string_view content_id) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    const auto item = std::ranges::find((*selected)->native->content, content_id, &ImageContentItem::id);
    if (item == (*selected)->native->content.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    const auto children = (*selected)->native->content_children.find(std::string{content_id});
    if (children == (*selected)->native->content_children.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    ImageContentScope result{*item, {}};
    result.children.reserve(children->second.size());
    for (const auto index : children->second)
        result.children.push_back((*selected)->native->content.at(index));
    return result;
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageObjectItem>>
axk::app::ImageSessionManager::objects(const ImageDocumentRef &target, std::string_view owner_id, std::size_t limit,
                                       std::optional<std::string_view> cursor,
                                       std::optional<std::string_view> object_type,
                                       std::optional<std::string_view> content_scope_id) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    if (!object_type && !content_scope_id)
        return implementation_->page((*selected)->native->objects, (*selected)->object_cursors, limit, cursor);

    const auto scope =
        "content:" + std::string{content_scope_id.value_or("")} + "\ntype:" + std::string{object_type.value_or("")};
    const std::vector<std::size_t> *indices = nullptr;
    static const std::vector<std::size_t> empty;
    if (content_scope_id) {
        const auto found = (*selected)->native->object_indices_by_content_scope.find(std::string{*content_scope_id});
        if (found == (*selected)->native->object_indices_by_content_scope.end())
            return std::unexpected(session_error("content_not_found", "content scope does not exist"));
        indices = &found->second;
    } else {
        const auto found = (*selected)->native->object_indices_by_type.find(std::string{*object_type});
        indices = found == (*selected)->native->object_indices_by_type.end() ? &empty : &found->second;
    }

    std::vector<std::size_t> filtered;
    if (content_scope_id && object_type) {
        filtered.reserve(indices->size());
        std::ranges::copy_if(*indices, std::back_inserter(filtered), [&](const std::size_t index) {
            return (*selected)->native->objects[index].type == *object_type;
        });
        indices = &filtered;
    }
    return implementation_->page((*selected)->native->objects, (*selected)->object_cursors, limit, cursor, scope,
                                 indices);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageRelationshipItem>>
axk::app::ImageSessionManager::relationships(const ImageDocumentRef &target, std::string_view owner_id,
                                             std::size_t limit, std::optional<std::string_view> cursor,
                                             ImageRelationshipFilter filter) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    if (!filter.content_scope_id && !filter.source_object_id && !filter.target_object_id && !filter.relationship_type)
        return implementation_->page((*selected)->native->relationships, (*selected)->relationship_cursors, limit,
                                     cursor);

    const auto scope = "content:" + std::string{filter.content_scope_id.value_or("")} +
                       "\nsource:" + std::string{filter.source_object_id.value_or("")} +
                       "\ntarget:" + std::string{filter.target_object_id.value_or("")} +
                       "\ntype:" + std::string{filter.relationship_type.value_or("")};
    const std::vector<std::size_t> *content_indices = nullptr;
    if (filter.content_scope_id) {
        const auto found =
            (*selected)->native->object_indices_by_content_scope.find(std::string{*filter.content_scope_id});
        if (found == (*selected)->native->object_indices_by_content_scope.end())
            return std::unexpected(session_error("content_not_found", "content scope does not exist"));
        content_indices = &found->second;
    }

    std::vector<std::size_t> indices;
    indices.reserve((*selected)->native->relationships.size());
    for (std::size_t index = 0U; index < (*selected)->native->relationships.size(); ++index) {
        const auto &item = (*selected)->native->relationships[index];
        if (filter.source_object_id && item.source_object_id != *filter.source_object_id)
            continue;
        if (filter.target_object_id && (!item.target_object_id || *item.target_object_id != *filter.target_object_id))
            continue;
        if (filter.relationship_type && item.type != *filter.relationship_type)
            continue;
        if (content_indices != nullptr) {
            const auto source = (*selected)->native->object_indices_by_id.find(item.source_object_id);
            if (source == (*selected)->native->object_indices_by_id.end() ||
                !std::ranges::binary_search(*content_indices, source->second)) {
                continue;
            }
        }
        indices.push_back(index);
    }
    return implementation_->page((*selected)->native->relationships, (*selected)->relationship_cursors, limit, cursor,
                                 scope, &indices);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageValidationItem>>
axk::app::ImageSessionManager::validation_issues(const ImageDocumentRef &target, std::string_view owner_id,
                                                 std::size_t limit, std::optional<std::string_view> cursor) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    return implementation_->page((*selected)->native->validation, (*selected)->validation_cursors, limit, cursor);
}

void axk::app::ImageSessionManager::cleanup() {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
}
