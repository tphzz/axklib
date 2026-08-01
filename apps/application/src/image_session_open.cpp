#include "image_sessions_internal.hpp"

#include "content_digest.hpp"

axk::app::Result<axk::app::ImageSessionSummary>
axk::app::ImageSessionManager::open(const ImageSourceRef &source, std::string owner_id,
                                    const CancellationToken &cancellation) {
    return open_with_companion_directories(source, std::move(owner_id), {}, cancellation);
}

axk::app::Result<axk::app::ImageSessionSummary>
axk::app::ImageSessionManager::open_with_companion_directories(const ImageSourceRef &source, std::string owner_id,
                                                               const std::vector<DirectoryRef> &companion_directories,
                                                               const CancellationToken &cancellation) {
    if (owner_id.empty())
        return std::unexpected(session_error("invalid_owner", "image session owner is required"));
    auto admission = implementation_->reserve_session();
    if (!admission)
        return std::unexpected(admission.error());
    const FileRef path_reference{source.root_id, source.relative_path};
    PathReservationCoordinator::Lease path_lease;
    PathReservationCoordinator::Lease companion_path_lease;
    if (implementation_->path_reservations != nullptr) {
        auto acquired = implementation_->path_reservations->try_acquire({path_reference, PathAccessMode::shared});
        if (!acquired)
            return std::unexpected(acquired.error());
        path_lease = std::move(*acquired);
    }
    std::shared_ptr<const RandomAccessReader> source_reader;
    std::function<Result<void>()> verify_source_unchanged;
    std::string target_snapshot_id;
    std::optional<MediaContainer> media;
    std::vector<DirectoryRef> matched_companion_directories;
    if (source.kind == ImageSourceKind::file) {
        const auto file = implementation_->sandbox.open_file(path_reference);
        if (!file)
            return std::unexpected(file.error());
        auto digest = detail::reader_sha256(*file->reader, cancellation);
        if (!digest)
            return std::unexpected(digest.error());
        target_snapshot_id = std::move(*digest);
        auto opened_media = axk::open_media(file->reader, std::filesystem::path{file->filename}, cancellation);
        if (!opened_media)
            return std::unexpected(core_error(opened_media.error(), source));
        media.emplace(std::move(*opened_media));
        source_reader = file->reader;
        verify_source_unchanged = file->verify_unchanged;
    } else {
        const DirectoryRef directory_reference{source.root_id, source.relative_path};
        auto tree = implementation_->sandbox.open_tree(
            directory_reference, {.maximum_entries = AxkObjectDirectory::maximum_entries,
                                  .maximum_total_file_bytes = AxkObjectDirectory::maximum_payload_bytes,
                                  .maximum_depth = AxkObjectDirectory::maximum_depth,
                                  .maximum_path_bytes = 64U * 1024U});
        if (!tree)
            return std::unexpected(tree.error());
        std::vector<AxkObjectDirectoryEntry> entries;
        std::vector<std::function<Result<void>()>> verifiers;
        entries.reserve(tree->entries().size());
        verifiers.reserve(tree->entries().size());
        for (std::size_t index = 0U; index < tree->entries().size(); ++index) {
            const auto &entry = tree->entries()[index];
            if (entry.kind != SandboxTreeEntryKind::file)
                continue;
            auto opened = tree->open_file(index);
            if (!opened)
                return std::unexpected(opened.error());
            entries.push_back({entry.relative_path, opened->reader});
            verifiers.push_back(std::move(opened->verify_unchanged));
        }
        auto directory = AxkObjectDirectory::open(entries, source.relative_path, cancellation);
        if (!directory)
            return std::unexpected(core_error(directory.error(), source));
        auto companions = append_required_companion_wave_data(implementation_->sandbox, source, *directory,
                                                              companion_directories, entries, verifiers);
        if (!companions)
            return std::unexpected(companions.error());
        matched_companion_directories = companions->directories;
        if (!companions->files.empty()) {
            directory = AxkObjectDirectory::open(std::move(entries), source.relative_path, cancellation);
            if (!directory)
                return std::unexpected(core_error(directory.error(), source));
            if (implementation_->path_reservations != nullptr) {
                std::vector<PathAccess> accesses;
                accesses.reserve(companions->files.size());
                std::ranges::transform(companions->files, std::back_inserter(accesses), [](const FileRef &reference) {
                    return PathAccess{reference, PathAccessMode::shared};
                });
                auto acquired = implementation_->path_reservations->try_acquire(accesses);
                if (!acquired)
                    return std::unexpected(acquired.error());
                companion_path_lease = std::move(*acquired);
            }
        }
        std::vector<std::byte> snapshot;
        for (const auto &object : directory->stored_objects()) {
            const auto append_text = [&](std::string_view value) {
                std::ranges::transform(value, std::back_inserter(snapshot),
                                       [](char ch) { return static_cast<std::byte>(ch); });
                snapshot.push_back(std::byte{0});
            };
            append_text(object.logical_path);
            const auto payload_size = static_cast<std::uint64_t>(object.raw_payload.size());
            for (std::size_t byte = 0U; byte < sizeof(payload_size); ++byte) {
                snapshot.push_back(static_cast<std::byte>((payload_size >> (byte * 8U)) & 0xffU));
            }
            snapshot.insert(snapshot.end(), object.raw_payload.begin(), object.raw_payload.end());
        }
        for (const auto &verify : verifiers) {
            if (const auto unchanged = verify(); !unchanged)
                return std::unexpected(
                    session_error("image_source_changed", "object directory changed while it was opened", true));
        }
        auto snapshot_reader = std::make_shared<MemoryReader>(std::move(snapshot));
        auto digest = detail::reader_sha256(*snapshot_reader, cancellation);
        if (!digest)
            return std::unexpected(digest.error());
        target_snapshot_id = std::move(*digest);
        source_reader = std::move(snapshot_reader);
        verify_source_unchanged = []() -> Result<void> { return {}; };
        media.emplace(std::move(*directory));
    }
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U,
                                                cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), source));
    auto graph = axk::build_relationship_graph(inventory->catalog);
    auto tree = axk::build_content_tree(*media, inventory->catalog, graph);
    std::unordered_map<std::uint8_t, std::string> partition_names;
    if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
        partition_names.reserve(sfs->partitions().size());
        for (const auto &partition : sfs->partitions())
            partition_names.emplace(partition.index.value, partition.name);
    }

    auto session = std::make_shared<Implementation::Session>();
    session->owner_id = std::move(owner_id);
    session->source = source;
    session->companion_directories = std::move(matched_companion_directories);
    session->source_reader = std::move(source_reader);
    session->verify_source_unchanged = std::move(verify_source_unchanged);
    session->target_snapshot_id = std::move(target_snapshot_id);
    session->format = media_kind_name(media->kind());
    session->root_count = tree.roots.size();
    session->last_access = implementation_->clock();
    session->path_lease = std::move(path_lease);
    session->companion_path_lease = std::move(companion_path_lease);

    std::unordered_map<std::string, std::string> object_ids;
    object_ids.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        auto identifier = random_identifier("object-");
        if (!identifier)
            return std::unexpected(identifier.error());
        object_ids.emplace(object.key, std::move(*identifier));
    }
    for (const auto &descriptor : inventory->objects)
        session->descriptors_by_id.emplace(object_ids.at(descriptor.key), descriptor);
    session->catalog_issues = inventory->catalog.issues;
    if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
        const auto orphan_report = axk::analyze_waveform_orphans(*sfs, inventory->catalog, graph);
        session->waveform_status_by_id.reserve(orphan_report.rows.size());
        for (const auto &row : orphan_report.rows)
            session->waveform_status_by_id.emplace(object_ids.at(row.object_key), row.status);
        session->waveform_cluster_counts_by_id.reserve(orphan_report.rows.size());
        for (const auto &object : inventory->catalog.objects) {
            if (object.object.header.type == axk::ObjectType::smpl) {
                session->waveform_cluster_counts_by_id.emplace(object_ids.at(object.key),
                                                               record_cluster_count(*sfs, object));
            }
        }
    }

    session->objects.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        ImageObjectItem item;
        item.id = object_ids.at(object.key);
        item.type = object.object.header.raw_type;
        item.name = object.object.header.name;
        item.format = object_format_name(object.object.format);
        item.stored_size_bytes = session->descriptors_by_id.at(item.id).size;
        if (object.placement) {
            item.partition_index = object.partition.value;
            item.partition_name = object.placement->partition_name;
            item.volume_name = object.placement->volume_name;
            item.category_name = object.placement->category_name;
            item.entry_name = object.placement->entry_name;
        }
        if (const auto *waveform = std::get_if<axk::CurrentSmpl>(&object.object.payload)) {
            const auto stored_width = waveform->stored_sample_width_bytes.value;
            item.waveform =
                WaveformMetadata{.sample_rate = waveform->sample_rate.value,
                                 .sample_width_bytes = waveform->stored_sample_width_bytes.value,
                                 .root_key = waveform->root_key.value,
                                 .fine_tune_cents = waveform->fine_tune_cents.value,
                                 .loop_mode = waveform->loop_mode.value,
                                 .loop_mode_label = waveform->loop_mode_label,
                                 .frame_count = stored_width == 0U ? 0U : waveform->stored_pcm_bytes / stored_width,
                                 .loop_start_frame = waveform->loop_start_frame.value,
                                 .loop_length_frames = waveform->loop_length_frames.value};
        }
        if (const auto *sequence = std::get_if<axk::CurrentSequence>(&object.object.payload)) {
            std::vector<SequenceMetadata::TempoEvent> tempo_events;
            tempo_events.reserve(sequence->tempo_events.size());
            for (const auto &event : sequence->tempo_events) {
                tempo_events.push_back(
                    {.tick = event.tick, .microseconds_per_quarter_note = event.microseconds_per_quarter_note});
            }
            item.sequence = SequenceMetadata{.format_version = sequence->format_version,
                                             .ticks_per_quarter_note = sequence->ticks_per_quarter_note,
                                             .first_tick = sequence->first_tick,
                                             .end_tick = sequence->end_tick,
                                             .event_count = sequence->event_count,
                                             .header_tempo_bpm = sequence->header_tempo_bpm,
                                             .effective_initial_tempo_microseconds_per_quarter_note =
                                                 sequence->effective_initial_tempo_microseconds_per_quarter_note,
                                             .tempo_events = std::move(tempo_events)};
        }
        session->object_indices_by_id[item.id] = session->objects.size();
        session->object_indices_by_type[item.type].push_back(session->objects.size());
        session->objects.push_back(std::move(item));
    }

    session->relationships.reserve(graph.relationships.size());
    for (const auto &relationship : graph.relationships) {
        const auto source_id = mapped_id(object_ids, relationship.source_key);
        if (!source_id)
            continue;
        ImageRelationshipItem item;
        auto relationship_id = random_identifier("relationship-");
        if (!relationship_id)
            return std::unexpected(relationship_id.error());
        item.id = std::move(*relationship_id);
        item.source_object_id = *source_id;
        if (relationship.target_key)
            item.target_object_id = mapped_id(object_ids, *relationship.target_key);
        for (const auto &candidate : relationship.candidate_keys) {
            if (const auto candidate_id = mapped_id(object_ids, candidate))
                item.candidate_object_ids.push_back(*candidate_id);
        }
        item.type = relationship.type;
        item.quality = relationship_quality_wire_name(relationship.quality);
        item.basis = relationship.basis;
        item.notes = relationship.notes;
        item.assignment_index = relationship.assignment_index;
        item.assignment_name = relationship.assignment_name;
        item.assignment_state = axk::assignment_state_name(relationship.assignment_state);
        item.receive_channel_display = relationship.receive_channel_display;
        session->relationships.push_back(std::move(item));
    }

    const auto append_content =
        [&](const auto &self, const axk::ContentNode &node, const std::optional<std::string> &parent_id,
            std::size_t depth,
            std::optional<std::uint8_t> inherited_partition_index) -> axk::app::Result<std::vector<std::size_t>> {
        auto generated_id = random_identifier("content-");
        if (!generated_id)
            return std::unexpected(generated_id.error());
        const auto id = std::move(*generated_id);
        const auto partition_index =
            node.node_type == "partition" ? partition_index_from_node_id(node.node_id) : inherited_partition_index;
        const auto canonical_name = [&]() -> std::string {
            if (node.node_type == "partition" && partition_index) {
                if (const auto found = partition_names.find(*partition_index); found != partition_names.end())
                    return found->second;
            }
            return node.display_name;
        }();
        ImageContentItem item{.id = id,
                              .parent_id = parent_id,
                              .depth = depth,
                              .partition_index = partition_index,
                              .kind = node.node_type,
                              .name = canonical_name,
                              .display_name = node.display_name,
                              .child_count = node.children.size(),
                              .object_id = std::nullopt,
                              .object_type = std::nullopt,
                              .scope_role = std::string{axk::content_scope_role_name(node.scope_role)},
                              .quality = std::string{axk::relationship_quality_name(node.quality)},
                              .basis = node.basis,
                              .notes = node.notes,
                              .details = node.details};
        if (!node.object_key.empty())
            item.object_id = mapped_id(object_ids, node.object_key);
        if (!node.object_type.empty())
            item.object_type = node.object_type;
        const auto item_index = session->content.size();
        session->content.push_back(std::move(item));
        session->content_children[parent_id.value_or("")].push_back(item_index);
        session->content_children.try_emplace(id);
        std::vector<std::size_t> scoped_indices;
        if (session->content[item_index].object_id) {
            if (const auto found = session->object_indices_by_id.find(*session->content[item_index].object_id);
                found != session->object_indices_by_id.end()) {
                scoped_indices.push_back(found->second);
            }
        }
        for (const auto &child : node.children) {
            auto appended = self(self, child, id, depth + 1U, partition_index);
            if (!appended)
                return std::unexpected(appended.error());
            scoped_indices.insert(scoped_indices.end(), appended->begin(), appended->end());
        }
        std::ranges::sort(scoped_indices);
        const auto unique_end = std::ranges::unique(scoped_indices).begin();
        scoped_indices.erase(unique_end, scoped_indices.end());
        if (session->content[item_index].kind == "volume" && !session->content[item_index].partition_index &&
            !scoped_indices.empty()) {
            const auto inferred_partition_index = session->objects[scoped_indices.front()].partition_index;
            if (inferred_partition_index && std::ranges::all_of(scoped_indices, [&](std::size_t object_index) {
                    return session->objects[object_index].partition_index == inferred_partition_index;
                })) {
                session->content[item_index].partition_index = inferred_partition_index;
            }
        }
        session->object_indices_by_content_scope.emplace(id, scoped_indices);
        if (node.scope_role == axk::ContentScopeRole::reference)
            return std::vector<std::size_t>{};
        return scoped_indices;
    };
    session->content_children.try_emplace("");
    for (const auto &root : tree.roots) {
        if (auto appended = append_content(append_content, root, std::nullopt, 0U, std::nullopt); !appended)
            return std::unexpected(appended.error());
    }

    const auto append_validation = [&](std::string code, std::string severity, std::string message,
                                       std::string sampler_path, std::optional<std::string> object_id) {
        session->validation.push_back(
            {std::move(code), std::move(severity), std::move(message), std::move(sampler_path), std::move(object_id)});
    };
    for (const auto &issue : tree.issues) {
        append_validation(issue.code, issue.severity, issue.message, issue.sampler_path,
                          issue.object_key.empty() ? std::nullopt : mapped_id(object_ids, issue.object_key));
    }
    for (const auto &issue : media->validation_issues())
        append_validation(issue.code, "warning", issue.message, issue.sampler_path, std::nullopt);
    if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
        const auto report = axk::validate_semantics(*sfs, inventory->catalog, graph);
        for (const auto &issue : report.issues) {
            std::string severity;
            switch (issue.severity) {
            case axk::ValidationSeverity::info:
                severity = "info";
                break;
            case axk::ValidationSeverity::warning:
                severity = "warning";
                break;
            case axk::ValidationSeverity::error:
                severity = "error";
                break;
            }
            append_validation(issue.code, std::move(severity), issue.message, issue.sampler_path,
                              issue.object_key.empty() ? std::nullopt : mapped_id(object_ids, issue.object_key));
        }
    }
    for (auto &object : inventory->catalog.objects) {
        const auto identifier = object_ids.at(object.key);
        session->snapshots_by_id.emplace(identifier, std::move(object));
    }
    session->media.emplace(std::move(*media));
    if (const auto unchanged = session->verify_source_unchanged(); !unchanged) {
        return std::unexpected(
            session_error("image_source_changed", "image source changed while the session was opened", true));
    }

    do {
        auto image_id = random_identifier("image-");
        if (!image_id)
            return std::unexpected(image_id.error());
        session->image_id = std::move(*image_id);
        if ((*admission)->promote(session))
            break;
    } while (true);
    return inspect(session->image_id, session->owner_id);
}

axk::app::Result<axk::app::ImageSessionSummary> axk::app::ImageSessionManager::attach_companion_directories(
    std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
    const CompanionDirectorySelection &selection, const CancellationToken &cancellation) {
    constexpr std::size_t maximum_selected_directories = 32U;
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());

    ImageSourceRef source;
    std::vector<DirectoryRef> current_directories;
    {
        const std::scoped_lock access{(*session)->access_mutex};
        if ((*session)->revision != expected_revision)
            return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
        if ((*session)->mutating)
            return std::unexpected(
                session_error("image_mutation_in_progress", "image session is being modified", true));
        if ((*session)->format != "axk-object-directory" ||
            (*session)->source.kind != ImageSourceKind::axk_object_directory) {
            return std::unexpected(
                session_error("companion_directories_unsupported",
                              "companion disk folders can only be attached to an AXK object directory session"));
        }
        source = (*session)->source;
        current_directories = (*session)->companion_directories;
    }

    std::vector<DirectoryRef> candidates;
    if (selection.kind == CompanionDirectorySelectionKind::immediate_siblings) {
        if (!selection.directories.empty())
            return std::unexpected(
                session_error("invalid_companion_directories",
                              "immediate sibling search does not accept explicit companion directory references"));
        auto siblings = immediate_sibling_directories(implementation_->sandbox, source);
        if (!siblings)
            return std::unexpected(siblings.error());
        candidates = std::move(*siblings);
    } else {
        if (selection.directories.empty() || selection.directories.size() > maximum_selected_directories) {
            return std::unexpected(
                session_error("invalid_companion_directories", "select between one and 32 companion disk folders"));
        }
        candidates = selection.directories;
    }
    std::vector<DirectoryRef> unique_candidates;
    unique_candidates.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        if (candidate.root_id.empty() ||
            (candidate.root_id == source.root_id && candidate.relative_path == source.relative_path) ||
            std::ranges::find(unique_candidates, candidate) != unique_candidates.end()) {
            continue;
        }
        unique_candidates.push_back(candidate);
    }
    if (unique_candidates.empty())
        return std::unexpected(
            session_error("companion_segment_not_found", "No selected folder contains matching Wave Data segments"));

    ImageSessionManager refreshed{implementation_->sandbox,
                                  1U,
                                  implementation_->maximum_page_size,
                                  implementation_->idle_retention,
                                  implementation_->clock,
                                  implementation_->path_reservations};
    auto opened =
        refreshed.open_with_companion_directories(source, std::string{owner_id}, unique_candidates, cancellation);
    if (!opened)
        return std::unexpected(opened.error());
    if (opened->companion_directories.empty()) {
        return std::unexpected(
            session_error("companion_segment_not_found", "No selected folder contains matching Wave Data segments"));
    }
    if (opened->companion_directories == current_directories)
        return inspect(image_id, owner_id);

    std::shared_ptr<Implementation::Session> fresh;
    {
        const std::scoped_lock lock{refreshed.implementation_->mutex};
        fresh = refreshed.implementation_->sessions.at(opened->image_id);
    }
    {
        const std::scoped_lock access{(*session)->access_mutex};
        if ((*session)->revision != expected_revision)
            return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
        if ((*session)->mutating)
            return std::unexpected(
                session_error("image_mutation_in_progress", "image session is being modified", true));
        Implementation::preserve_object_ids(**session, *fresh);
        Implementation::adopt_refreshed_state(**session, *fresh);
        ++(*session)->revision;
    }
    return inspect(image_id, owner_id);
}
