#include "image_sessions_internal.hpp"

#include "axklib/bytes.hpp"
#include "axklib/sfs_repair.hpp"

axk::app::Result<axk::app::ImageSessionSummary> axk::app::ImageSessionManager::inspect(std::string_view image_id,
                                                                                       std::string_view owner_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    ImageValidationSummary validation;
    for (const auto &issue : (*session)->validation) {
        if (issue.severity == "error")
            ++validation.error_count;
        else if (issue.severity == "warning")
            ++validation.warning_count;
        else
            ++validation.info_count;
    }
    std::vector<std::string> available_operations{
        "images.content",           "images.objects",         "images.relationships", "images.systemProgramContexts",
        "images.validation.issues", "images.preview",         "auditions.prepare",    "images.package.export",
        "images.audio_export",      "images.sequence_export",
    };
    if ((*session)->format == "sfs" || (*session)->format == "iso9660")
        available_operations.emplace_back("images.volume_package_export");
    if ((*session)->format == "sfs" && (*session)->source.kind == ImageSourceKind::file) {
        available_operations.emplace_back("images.volume_floppy_export");
        available_operations.emplace_back("images.media_conversion");
    }
    const auto source_metadata =
        implementation_->sandbox.metadata((*session)->source.root_id, (*session)->source.relative_path);
    const auto *mutable_container = (*session)->media ? std::get_if<Container>(&(*session)->media->storage()) : nullptr;
    if ((*session)->format == "sfs" && (*session)->source.kind == ImageSourceKind::file &&
        mutable_container != nullptr && inspect_sfs_extent_layout_repair(*mutable_container)) {
        available_operations.emplace_back("images.extent_layout.repair");
    }
    const auto supported_mutation_profile =
        mutable_container != nullptr && mutable_container->superblock().sector_size_bytes == 512U &&
        std::ranges::all_of(mutable_container->partitions(), [](const Partition &partition) {
            return partition.sectors_per_cluster == 2U && allocation_is_safe_for_mutation(partition.allocation);
        });
    if ((*session)->format == "sfs" && supported_mutation_profile && source_metadata && source_metadata->writable) {
        available_operations.emplace_back("images.alter.volumes");
        available_operations.emplace_back("images.alter.partitions");
        available_operations.emplace_back("images.alter.objects");
        available_operations.emplace_back("images.package.import");
        available_operations.emplace_back("images.deletion.orphans.inspect");
        available_operations.emplace_back("images.programs.generate.inspect");
        available_operations.emplace_back("images.programs.generate");
    }
    return ImageSessionSummary{.image_id = (*session)->image_id,
                               .revision = (*session)->revision,
                               .source = (*session)->source,
                               .companion_sources = (*session)->companion_sources,
                               .floppy_set = (*session)->floppy_set,
                               .format = (*session)->format,
                               .available_operations = std::move(available_operations),
                               .root_count = (*session)->root_count,
                               .object_count = (*session)->objects.size(),
                               .relationship_count = (*session)->relationships.size(),
                               .validation = validation};
}

axk::app::Result<axk::app::ImageObjectDeletionPlan> axk::app::ImageSessionManager::plan_deletion(
    std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
    const std::vector<std::string> &target_object_ids, const std::vector<std::string> &cleanup_object_ids) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    if ((*session)->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if ((*session)->format != "sfs" || !(*session)->media)
        return std::unexpected(
            session_error("image_mutation_unsupported", "only SFS image sessions support object deletion"));
    std::vector<std::string> target_keys;
    target_keys.reserve(target_object_ids.size());
    for (const auto &object_id : target_object_ids) {
        const auto found = (*session)->snapshots_by_id.find(object_id);
        if (found == (*session)->snapshots_by_id.end())
            return std::unexpected(session_error("object_not_found", "deletion target does not exist"));
        target_keys.push_back(found->second.key);
    }
    std::vector<std::string> cleanup_keys;
    cleanup_keys.reserve(cleanup_object_ids.size());
    for (const auto &object_id : cleanup_object_ids) {
        const auto found = (*session)->snapshots_by_id.find(object_id);
        if (found == (*session)->snapshots_by_id.end()) {
            return std::unexpected(session_error("object_not_found", "deletion cleanup object does not exist"));
        }
        cleanup_keys.push_back(found->second.key);
    }
    ObjectCatalog catalog;
    catalog.issues = (*session)->catalog_issues;
    catalog.objects.reserve((*session)->snapshots_by_id.size());
    for (const auto &[id, snapshot] : (*session)->snapshots_by_id) {
        static_cast<void>(id);
        catalog.objects.push_back(snapshot);
    }
    std::ranges::sort(catalog.objects, {}, [](const auto &object) { return object.key; });
    const auto graph = build_relationship_graph(catalog);
    const auto *container = std::get_if<Container>(&(*session)->media->storage());
    if (container == nullptr)
        return std::unexpected(
            session_error("image_mutation_unsupported", "object deletion requires an SFS container"));
    const auto inspected = inspect_object_deletion(
        *container, catalog, graph, {.target_keys = std::move(target_keys), .cleanup_keys = std::move(cleanup_keys)});
    if (!inspected)
        return std::unexpected(session_error("deletion_invalid", inspected.error().message));

    std::unordered_map<std::string, std::string> ids_by_key;
    ids_by_key.reserve((*session)->snapshots_by_id.size());
    for (const auto &[id, snapshot] : (*session)->snapshots_by_id)
        ids_by_key.emplace(snapshot.key, id);
    const auto id_for = [&](std::string_view key) -> std::optional<std::string> {
        if (const auto found = ids_by_key.find(std::string{key}); found != ids_by_key.end())
            return found->second;
        return std::nullopt;
    };
    const auto map_keys = [&](const std::vector<std::string> &keys) {
        std::vector<std::string> result;
        result.reserve(keys.size());
        for (const auto &key : keys) {
            if (auto id = id_for(key))
                result.push_back(std::move(*id));
        }
        return result;
    };

    ImageObjectDeletionInspection result;
    result.can_apply = inspected->can_apply;
    result.image_id = std::string{image_id};
    result.revision = expected_revision;
    result.target_object_ids = target_object_ids;
    result.selected_object_ids = map_keys(inspected->selected_keys);
    result.estimated_freed_bytes = inspected->estimated_freed_bytes;
    result.estimated_freed_clusters = inspected->estimated_freed_clusters;
    result.impacts.reserve(inspected->impacts.size());
    for (const auto &impact : inspected->impacts) {
        const auto object_id = id_for(impact.object_key);
        if (!object_id)
            continue;
        result.impacts.push_back({.object_id = *object_id,
                                  .object_type = std::string{object_type_name(impact.object_type)},
                                  .object_name = impact.object_name,
                                  .partition_index = impact.partition.value,
                                  .partition_name = impact.partition_name,
                                  .volume_name = impact.volume_name,
                                  .role = std::string{object_deletion_role_name(impact.role)},
                                  .status = std::string{object_deletion_status_name(impact.status)},
                                  .selected = impact.selected,
                                  .stored_size_bytes = impact.stored_size_bytes,
                                  .freed_clusters = impact.freed_clusters,
                                  .prerequisite_object_ids = map_keys(impact.prerequisite_keys),
                                  .reason = impact.reason});
    }
    result.references.reserve(inspected->references.size());
    for (const auto &reference : inspected->references) {
        const auto source_id = id_for(reference.source_key);
        if (!source_id)
            continue;
        const auto source = std::ranges::find(catalog.objects, reference.source_key, &ObjectSnapshot::key);
        const auto target_object = reference.target_key.empty()
                                       ? catalog.objects.end()
                                       : std::ranges::find(catalog.objects, reference.target_key, &ObjectSnapshot::key);
        result.references.push_back(
            {.source_object_id = *source_id,
             .source_object_type = source == catalog.objects.end()
                                       ? "Unknown"
                                       : std::string{object_type_name(source->object.header.type)},
             .source_object_name = source == catalog.objects.end() ? std::string{} : source->object.header.name,
             .target_object_id = id_for(reference.target_key),
             .target_object_type = target_object == catalog.objects.end() ? std::nullopt
                                                                          : std::optional<std::string>{object_type_name(
                                                                                target_object->object.header.type)},
             .target_object_name = target_object == catalog.objects.end()
                                       ? std::nullopt
                                       : std::optional<std::string>{target_object->object.header.name},
             .type = reference.type,
             .quality = std::string{relationship_quality_name(reference.quality)},
             .effect = std::string{object_deletion_reference_effect_name(reference.effect)}});
    }
    const auto append_notices = [&](const std::vector<ObjectDeletionNotice> &source,
                                    std::vector<ImageObjectDeletionNotice> &destination) {
        destination.reserve(source.size());
        for (const auto &notice : source)
            destination.push_back({notice.code, notice.message, map_keys(notice.object_keys)});
    };
    append_notices(inspected->blockers, result.blockers);
    append_notices(inspected->warnings, result.warnings);
    return ImageObjectDeletionPlan{std::move(result), std::move(inspected->manifest)};
}

axk::app::Result<axk::app::ImageWaveDataOrphanInspection> axk::app::ImageSessionManager::inspect_wave_data_orphans(
    std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
    std::string_view content_scope_id, std::size_t maximum_candidates) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    if ((*session)->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if ((*session)->format != "sfs" || !(*session)->media)
        return std::unexpected(
            session_error("image_mutation_unsupported", "Wave Data cleanup requires an SFS image session"));
    const auto content_item = std::ranges::find((*session)->content, content_scope_id, &axk::app::ImageContentItem::id);
    if (content_item == (*session)->content.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    if (content_item->kind != "volume")
        return std::unexpected(
            session_error("content_scope_invalid", "Wave Data cleanup requires a volume content scope"));
    const auto scope = (*session)->object_indices_by_content_scope.find(std::string{content_scope_id});
    if (scope == (*session)->object_indices_by_content_scope.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    const auto *container = std::get_if<Container>(&(*session)->media->storage());
    if (container == nullptr)
        return std::unexpected(
            session_error("image_mutation_unsupported", "Wave Data cleanup requires an SFS container"));

    ImageWaveDataOrphanInspection result;
    result.image_id = std::string{image_id};
    result.revision = expected_revision;
    result.content_scope_id = std::string{content_scope_id};
    const auto candidate_limit = std::min(maximum_candidates, std::size_t{1024U});
    const auto candidate_less = [](const auto &left, const auto &right) {
        if (left.partition_index != right.partition_index)
            return left.partition_index < right.partition_index;
        if (left.volume_name != right.volume_name)
            return left.volume_name < right.volume_name;
        if (left.object_name != right.object_name)
            return left.object_name < right.object_name;
        return left.object_id < right.object_id;
    };
    for (const auto object_index : scope->second) {
        if (object_index >= (*session)->objects.size())
            return std::unexpected(
                session_error("image_session_invalid", "content scope references an invalid object"));
        const auto &item = (*session)->objects[object_index];
        if (item.type != "SMPL")
            continue;
        const auto status = (*session)->waveform_status_by_id.find(item.id);
        if (status == (*session)->waveform_status_by_id.end() || status->second != WaveformStatus::known_unreferenced) {
            continue;
        }
        if (!item.partition_index)
            return std::unexpected(session_error("image_session_invalid", "Wave Data partition is unavailable"));
        const auto partition = std::ranges::find(container->partitions(), *item.partition_index,
                                                 [](const Partition &value) { return value.index.value; });
        if (partition == container->partitions().end())
            return std::unexpected(session_error("image_session_invalid", "Wave Data partition is unavailable"));
        const auto cluster_count = (*session)->waveform_cluster_counts_by_id.find(item.id);
        if (cluster_count == (*session)->waveform_cluster_counts_by_id.end())
            return std::unexpected(session_error("image_session_invalid", "Wave Data allocation is unavailable"));
        const auto clusters = cluster_count->second;
        const auto cluster_bytes =
            checked_multiply(container->superblock().sector_size_bytes, partition->sectors_per_cluster);
        if (!cluster_bytes)
            return std::unexpected(session_error("image_session_invalid", cluster_bytes.error().message));
        const auto recoverable_bytes = checked_multiply(clusters, *cluster_bytes);
        if (!recoverable_bytes)
            return std::unexpected(session_error("image_session_invalid", recoverable_bytes.error().message));
        ++result.total_candidate_count;
        if (candidate_limit == 0U)
            continue;
        ImageWaveDataOrphanCandidate candidate{.object_id = item.id,
                                               .object_type = item.type,
                                               .object_name = item.name,
                                               .partition_index = item.partition_index,
                                               .partition_name = item.partition_name,
                                               .volume_name = item.volume_name,
                                               .stored_size_bytes = item.stored_size_bytes,
                                               .recoverable_bytes = *recoverable_bytes,
                                               .recoverable_clusters = clusters};
        if (result.candidates.size() < candidate_limit) {
            result.candidates.push_back(std::move(candidate));
            std::ranges::push_heap(result.candidates, candidate_less);
        } else if (candidate_less(candidate, result.candidates.front())) {
            std::ranges::pop_heap(result.candidates, candidate_less);
            result.candidates.back() = std::move(candidate);
            std::ranges::push_heap(result.candidates, candidate_less);
        }
    }
    std::ranges::sort_heap(result.candidates, candidate_less);
    return result;
}
