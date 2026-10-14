#include "formats/a_series/session_state.hpp"
#include "image_sessions_internal.hpp"
#include "support.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <map>
#include <set>

#include "axklib/application/formats/a_series/program_formats.hpp"
#include "axklib/application/formats/a_series/sample_formats.hpp"
#include "axklib/diagnostics.hpp"
#include "axklib/package_closure.hpp"

namespace {

std::optional<std::uint64_t>
exact_dependency_size(const axk::ObjectSnapshot &root, const axk::RelationshipGraph &graph,
                      const std::map<std::string, const axk::ObjectSnapshot *, std::less<>> &objects,
                      const std::map<std::string, std::uint64_t, std::less<>> &sizes) {
    if (root.object.header.type != axk::ObjectType::prog && root.object.header.type != axk::ObjectType::sbac &&
        root.object.header.type != axk::ObjectType::sbnk) {
        return std::nullopt;
    }
    std::set<std::string, std::less<>> visited;
    std::set<std::string, std::less<>> active;
    std::uint64_t total{};
    const auto visit = [&](const auto &self, const axk::ObjectSnapshot &object) -> bool {
        const auto has_exact_closure_payload = [&]() {
            switch (object.object.header.type) {
            case axk::ObjectType::prog:
                return std::holds_alternative<axk::CurrentProg>(object.object.payload);
            case axk::ObjectType::sbac:
                return std::holds_alternative<axk::CurrentSbac>(object.object.payload);
            case axk::ObjectType::sbnk:
                return std::holds_alternative<axk::CurrentSbnk>(object.object.payload);
            default:
                return true;
            }
        };
        if (!has_exact_closure_payload())
            return false;
        if (active.contains(object.key))
            return false;
        if (!visited.emplace(object.key).second)
            return true;
        const auto size = sizes.find(object.key);
        if (size == sizes.end() || size->second > std::numeric_limits<std::uint64_t>::max() - total)
            return false;
        total += size->second;
        active.emplace(object.key);
        auto required = axk::package_internal::required_relationships(object, graph, objects);
        if (!required)
            return false;
        for (const auto *relationship : *required) {
            const auto target = relationship->target_key ? objects.find(*relationship->target_key) : objects.end();
            if (target == objects.end() || !self(self, *target->second))
                return false;
        }
        active.erase(object.key);
        return true;
    };
    return visit(visit, root) ? std::optional{total} : std::nullopt;
}

} // namespace

axk::app::Result<std::unique_ptr<axk::app::formats::a_series::SessionState>>
axk::app::formats::a_series::SessionState::create(std::shared_ptr<const axk::formats::a_series::Document> document,
                                                  const ImageSourceRef &source, diagnostic_sink *diagnostics) {
    auto session = std::make_unique<SessionState>();
    const auto &native_state = document->state();
    const auto *inventory = &native_state.inventory;
    const auto &graph = native_state.graph;
    const auto &tree = native_state.tree;
    const auto admitted_media = std::shared_ptr<const MediaContainer>{document, &native_state.media};
    (*session).document = std::move(document);
    (*session).source_reference = source;
    (*session).media = admitted_media;
    session->root_count = tree.roots.size();
    axk::DiagnosticTimer indexes_timer{diagnostics, diagnostic_source::objects, "image_open_phase", "session-indexes"};
    std::unordered_map<std::uint8_t, std::string> partition_names;
    std::unordered_map<std::uint8_t, ImagePartitionCapacity> partition_capacities;
    if (const auto *sfs = std::get_if<axk::Container>(&admitted_media->storage())) {
        partition_names.reserve(sfs->partitions().size());
        partition_capacities.reserve(sfs->partitions().size());
        for (const auto &partition : sfs->partitions()) {
            partition_names.emplace(partition.index.value, partition.name);
            if (partition.allocation.free_space) {
                const auto &free_space = *partition.allocation.free_space;
                partition_capacities.emplace(
                    partition.index.value,
                    ImagePartitionCapacity{.allocated_clusters = free_space.allocated_cluster_count,
                                           .free_clusters = free_space.free_cluster_count,
                                           .cluster_size_bytes = free_space.cluster_size_bytes});
            }
        }
    }

    std::unordered_map<std::string, std::string> object_ids;
    object_ids.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        auto identifier = random_identifier("object-");
        if (!identifier)
            return std::unexpected(identifier.error());
        object_ids.emplace(object.key, std::move(*identifier));
    }
    for (const auto &descriptor : inventory->objects)
        (*session).descriptors_by_id.emplace(object_ids.at(descriptor.key), &descriptor);
    std::map<std::string, const axk::ObjectSnapshot *, std::less<>> objects_by_key;
    std::map<std::string, std::uint64_t, std::less<>> sizes_by_key;
    for (const auto &object : inventory->catalog.objects)
        objects_by_key.emplace(object.key, &object);
    for (const auto &descriptor : inventory->objects)
        sizes_by_key.emplace(descriptor.key, descriptor.size);
    (*session).catalog_issues = inventory->catalog.issues;
    if (const auto *sfs = std::get_if<axk::Container>(&admitted_media->storage())) {
        const auto orphan_report = axk::analyze_waveform_orphans(*sfs, inventory->catalog, graph);
        (*session).waveform_status_by_id.reserve(orphan_report.rows.size());
        for (const auto &row : orphan_report.rows)
            (*session).waveform_status_by_id.emplace(object_ids.at(row.object_key), row.status);
        (*session).waveform_cluster_counts_by_id.reserve(orphan_report.rows.size());
        for (const auto &object : inventory->catalog.objects) {
            if (object.object.header.type == axk::ObjectType::smpl) {
                (*session).waveform_cluster_counts_by_id.emplace(object_ids.at(object.key),
                                                                 record_cluster_count(*sfs, object));
            }
        }
    }

    std::map<std::string, std::vector<std::size_t>, std::less<>> owned_volume_objects;
    session->objects.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        ImageObjectItem item;
        item.id = object_ids.at(object.key);
        item.type = object.object.header.raw_type;
        item.name = object.object.header.name;
        item.format = object_format_name(object.object.format);
        item.stored_size_bytes = (*session).descriptors_by_id.at(item.id)->size;
        item.size_with_dependencies_bytes = exact_dependency_size(object, graph, objects_by_key, sizes_by_key);
        if (object.placement) {
            item.partition_index = object.partition.value;
            item.partition_name = object.placement->partition_name;
            item.volume_name = object.placement->volume_name;
            item.category_name = object.placement->category_name;
            item.entry_name = object.placement->entry_name;
            owned_volume_objects[std::format("volume:{}:{}", object.partition.value,
                                             object.placement->volume_directory.value)]
                .push_back(session->objects.size());
        }
        if (const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload))
            item.sample_format = sample_format_metadata(*sample);
        if (const auto *bank = std::get_if<axk::CurrentSbac>(&object.object.payload))
            item.sample_format = sample_format_metadata(*bank);
        item.program_format = program_format_metadata(object.object);
        if (const auto *waveform = std::get_if<axk::CurrentSmpl>(&object.object.payload)) {
            const auto stored_width = waveform->stored_sample_width_bytes.value;
            item.waveform = WaveformMetadata{
                .sample_rate = waveform->sample_rate.value,
                .sample_width_bytes = waveform->stored_sample_width_bytes.value,
                .embedded_container_name = waveform->embedded_container_name.value,
                .root_key = waveform->root_key.value,
                .fine_tune_cents = waveform->fine_tune_cents.value,
                .loop_mode = waveform->loop_mode.value,
                .loop_mode_label = waveform->loop_mode_label,
                .stored_frame_count = stored_width == 0U ? 0U : waveform->stored_pcm_bytes / stored_width,
                .wave_start_frame = waveform->wave_start_frame.value,
                .wave_length_frames = waveform->wave_length_frames.value,
                .loop_start_frame = waveform->loop_start_frame.value,
                .loop_length_frames = waveform->loop_length_frames.value,
                .storage_state = waveform->stored_segment_offset == 0U &&
                                         waveform->stored_segment_bytes == waveform->stored_pcm_bytes
                                     ? "COMPLETE"
                                     : "INCOMPLETE"};
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
        const auto volume_directory_id = [&]() -> std::optional<std::uint32_t> {
            if (node.node_type != "volume")
                return std::nullopt;
            const auto separator = node.node_id.rfind(':');
            if (separator == std::string::npos || separator + 1U == node.node_id.size())
                return std::nullopt;
            std::uint32_t result{};
            const auto first = node.node_id.data() + static_cast<std::ptrdiff_t>(separator + 1U);
            const auto last = node.node_id.data() + static_cast<std::ptrdiff_t>(node.node_id.size());
            const auto converted = std::from_chars(first, last, result);
            return converted.ec == std::errc{} && converted.ptr == last ? std::optional{result} : std::nullopt;
        }();
        const auto partition_capacity = [&]() -> std::optional<ImagePartitionCapacity> {
            if (node.node_type != "partition" || !partition_index)
                return std::nullopt;
            if (const auto found = partition_capacities.find(*partition_index); found != partition_capacities.end())
                return found->second;
            return std::nullopt;
        }();
        ImageContentItem item{.id = id,
                              .parent_id = parent_id,
                              .depth = depth,
                              .partition_index = partition_index,
                              .volume_directory_id = volume_directory_id,
                              .partition_capacity = partition_capacity,
                              .size_bytes = std::nullopt,
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
        // Device navigation may hide quiet default Programs, but inventory must retain their real records.
        if (node.node_type == "volume") {
            if (const auto found = owned_volume_objects.find(node.node_id); found != owned_volume_objects.end())
                scoped_indices.insert(scoped_indices.end(), found->second.begin(), found->second.end());
        }
        std::ranges::sort(scoped_indices);
        const auto unique_end = std::ranges::unique(scoped_indices).begin();
        scoped_indices.erase(unique_end, scoped_indices.end());
        if (session->content[item_index].kind == "volume") {
            std::uint64_t size{};
            const auto complete = std::ranges::all_of(scoped_indices, [&](std::size_t object_index) {
                const auto object_size = session->objects[object_index].stored_size_bytes;
                if (object_size > std::numeric_limits<std::uint64_t>::max() - size)
                    return false;
                size += object_size;
                return true;
            });
            if (complete)
                session->content[item_index].size_bytes = size;
        }
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
    indexes_timer.finish();
    const auto &validation = native_state.validation;
    for (const auto &issue : validation.issues) {
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
    for (const auto &object : inventory->catalog.objects) {
        const auto identifier = object_ids.at(object.key);
        (*session).snapshots_by_id.emplace(identifier, &object);
        (*session).native_object_ids.emplace(object.key, identifier);
    }
    return session;
}
