#include "image_sessions_internal.hpp"

axk::app::Result<axk::app::ImageSessionMutation>
axk::app::ImageSessionManager::begin_mutation(const ImageDocumentRef &target, std::string_view owner_id,
                                              std::uint64_t expected_revision, std::string_view operation_id) {
    auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    std::string member_id;
    {
        const std::scoped_lock access{(*session)->access_mutex};
        auto selected = Implementation::document(**session, target.document_id);
        if (!selected)
            return std::unexpected(selected.error());
        if ((*selected)->native->format_id() != "yamaha.a-series" || (*selected)->member_ids.size() != 1U)
            return std::unexpected(session_error("image_mutation_unsupported", "Select one writable native source"));
        member_id = (*selected)->member_ids.front();
    }
    return begin_mutation_access({target.image_id, member_id}, owner_id, expected_revision, std::nullopt, operation_id);
}

axk::app::Result<axk::app::ImageSessionMutation>
axk::app::ImageSessionManager::begin_filesystem_mutation(const ImageSourceTarget &target, std::string_view owner_id,
                                                         std::uint64_t expected_revision, PartitionIndex partition,
                                                         std::string_view operation_id) {
    return begin_mutation_access(target, owner_id, expected_revision, partition, operation_id);
}

axk::app::Result<axk::app::ImageSessionMutation> axk::app::ImageSessionManager::begin_mutation_access(
    const ImageSourceTarget &source_target, std::string_view owner_id, std::uint64_t expected_revision,
    std::optional<PartitionIndex> filesystem_partition, std::string_view operation_id) {
    const auto session = implementation_->owned(source_target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    auto access = std::unique_lock{(*session)->access_mutex};
    auto member = Implementation::member(**session, source_target.member_id);
    if (!member)
        return std::unexpected(member.error());
    if ((*session)->invalidated)
        return std::unexpected(
            session_error("image_session_invalidated", "image session requires recovery and reopening"));
    if ((*session)->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if (!filesystem_partition && (*member)->format != "sfs")
        return std::unexpected(session_error("image_mutation_unsupported", "only SFS image sessions can be altered"));
    const auto *container = (*member)->media ? std::get_if<Container>(&(*member)->media->storage()) : nullptr;
    if (!(*member)->media)
        return std::unexpected(session_error("image_media_unavailable", "image session media is unavailable", true));
    if (filesystem_partition && !container) {
        const auto &storage = (*member)->media->storage();
        const auto *fat = std::get_if<FatImage>(&storage);
        if (const auto *disk = std::get_if<FatDiskImage>(&storage)) {
            if (filesystem_partition->value >= disk->partitions().size())
                return std::unexpected(
                    session_error("filesystem_partition_not_found", "filesystem partition does not exist"));
            fat = &disk->partitions()[filesystem_partition->value].volume;
        } else if (filesystem_partition->value != 0U) {
            return std::unexpected(
                session_error("filesystem_partition_not_found", "filesystem partition does not exist"));
        }
        if (!fat || fat->geometry().profile == FatProfile::a_series_floppy)
            return std::unexpected(
                session_error("image_mutation_unsupported", "raw filesystem editing requires SFS or supported FAT16"));
    } else if (filesystem_partition) {
        const auto selected = std::ranges::find(container->partitions(), *filesystem_partition, &Partition::index);
        if (selected == container->partitions().end())
            return std::unexpected(
                session_error("filesystem_partition_not_found", "filesystem partition does not exist"));
        if (!container->backup_superblock_matches() || !selected->backup_header_matches ||
            !allocation_is_safe_for_mutation(selected->allocation) || !locate_partition_root_record(*selected))
            return std::unexpected(
                session_error("image_integrity_unsafe", "filesystem allocation metadata is inconsistent"));
    } else {
        if (!container)
            return std::unexpected(session_error("image_media_unavailable", "SFS image media is unavailable", true));
        if (container->superblock().sector_size_bytes != 512U ||
            !std::ranges::all_of(container->partitions(),
                                 [](const Partition &partition) { return partition.sectors_per_cluster == 2U; })) {
            return std::unexpected(session_error(
                "image_mutation_unsupported", "image geometry is outside the supported 512-byte alteration profile"));
        }
        if (!std::ranges::all_of(container->partitions(), [](const Partition &partition) {
                return allocation_is_safe_for_mutation(partition.allocation) &&
                       locate_partition_root_record(partition).has_value();
            })) {
            return std::unexpected(session_error(
                "image_integrity_unsafe",
                "image allocation metadata is inconsistent; alteration is disabled to avoid further damage"));
        }
    }
    if ((*session)->mutating)
        return std::unexpected(session_error("entry_in_use", "image session mutation is already active", true));
    if (auto unchanged = (*member)->verify_unchanged(); !unchanged)
        return std::unexpected(session_error("image_source_changed", "image source changed after it was opened", true));
    const auto source = (*member)->reference;
    Result<std::shared_ptr<MutationTarget>> target;
    if (source.kind == ImageSourceKind::device) {
        if (!implementation_->devices || (*member)->format != "sfs")
            return std::unexpected(session_error("device_read_only", "this device target is read-only"));
        target = implementation_->devices->begin_mutation(source.device_session_id, source.device_target_id, owner_id,
                                                          operation_id);
    } else {
        if (!(*member)->reservation)
            return std::unexpected(
                session_error("image_mutation_unsupported", "Source has no retained write reservation"));
        if (auto upgraded = (*member)->reservation->try_upgrade(); !upgraded)
            return std::unexpected(upgraded.error());
        auto file = implementation_->sandbox.open_mutation({source.root_id, source.relative_path});
        if (file)
            target = std::move(*file);
        else
            target = std::unexpected(file.error());
    }
    if (!target) {
        if ((*member)->reservation)
            (*member)->reservation->downgrade();
        return std::unexpected(target.error());
    }
    // Device admission already verified fresh metadata under its retained lock.
    // File targets still require their own unchanged-source check here.
    if ((*target)->revision() != (*member)->revision ||
        (source.kind != ImageSourceKind::device && !(*target)->verify_unchanged())) {
        if ((*member)->reservation)
            (*member)->reservation->downgrade();
        return std::unexpected(session_error("image_source_changed", "image source changed before mutation", true));
    }
    (*session)->mutation_member_id = (*member)->id;
    (*session)->mutating = true;
    (*session)->mutation_guard.emplace(std::move(access));
    return ImageSessionMutation{(*session)->image_id, (*session)->revision,     std::move(source),
                                std::move(*target),   (*member)->media->kind(), (*member)->id};
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

    ImageSessionManager refreshed{implementation_->sandbox,
                                  1U,
                                  implementation_->maximum_page_size,
                                  implementation_->idle_retention,
                                  implementation_->clock,
                                  nullptr,
                                  implementation_->maximum_audition_bundle_bytes,
                                  implementation_->maximum_audition_clips,
                                  implementation_->devices};
    ImageOpenSources request;
    for (const auto &source : session->sources.members) {
        if (source->id != session->mutation_member_id)
            if (auto checked = source->verify_unchanged(); !checked)
                return std::unexpected(checked.error());
        request.sources.push_back(source->reference);
    }
    request.selected_formats = session->selected_formats;
    auto opened = refreshed.open(request, std::string{owner_id}, cancellation);
    if (!opened)
        return std::unexpected(opened.error());
    std::shared_ptr<Implementation::Session> fresh;
    {
        const std::scoped_lock lock{refreshed.implementation_->mutex};
        fresh = refreshed.implementation_->sessions.at(opened->image_id);
    }
    if (auto preserved = Implementation::preserve_documents(*fresh, *session, session->mutation_member_id); !preserved)
        return std::unexpected(preserved.error());
    for (const auto &source : session->sources.members)
        if (source->id != session->mutation_member_id)
            if (auto checked = source->verify_unchanged(); !checked)
                return std::unexpected(checked.error());
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
    const auto member = Implementation::member(*session, session->mutation_member_id);
    if (member && (*member)->reservation)
        (*member)->reservation->downgrade();
    session->mutation_member_id.clear();
    session->mutation_guard.reset();
    return std::move(prepared.summary);
}

axk::app::Result<void> axk::app::ImageSessionManager::refresh_rolled_back_mutation(std::string_view image_id,
                                                                                   std::string_view owner_id,
                                                                                   std::uint64_t expected_revision) {
    auto prepared = prepare_mutation_commit(image_id, owner_id, expected_revision);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto current = std::static_pointer_cast<Implementation::Session>(prepared->current_state);
    auto fresh = std::static_pointer_cast<Implementation::Session>(prepared->refreshed_state);
    // Rebind containers as well as their access metadata: device readers from
    // before a write are invalid even after byte-exact restoration.
    Implementation::adopt_refreshed_state(*current, *fresh);
    return {};
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
                                                   std::uint64_t expected_revision, bool invalidate_session) noexcept {
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
    session->invalidated = invalidate_session;
    if (invalidate_session)
        for (const auto &document : session->documents)
            document->native->clear_transient_state();
    session->mutating = false;
    const auto member = Implementation::member(*session, session->mutation_member_id);
    if (member && (*member)->reservation)
        (*member)->reservation->downgrade();
    session->mutation_member_id.clear();
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
