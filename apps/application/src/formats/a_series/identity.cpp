#include "axklib/utf8.hpp"
#include "formats/a_series/session_state.hpp"
#include "image_sessions_internal.hpp"

axk::app::Result<void>
axk::app::formats::a_series::SessionState::preserve_identity(const NativeDocumentState &previous) {
    const auto *prior = dynamic_cast<const SessionState *>(&previous);
    if (prior == nullptr)
        return std::unexpected(session_error("native_format_changed", "The refreshed document changed format"));
    const auto &current = *prior;
    auto &fresh = *this;
    const auto stable_key = [](const SessionState &session, const ObjectSnapshot &snapshot) {
        if (session.media->kind() == MediaKind::sfs) {
            return std::format("{}:{}:{}", snapshot.partition.value, snapshot.sfs_id.value,
                               snapshot.object.header.raw_type);
        }
        if (session.media->kind() == MediaKind::axk_object_directory) {
            const auto separator = snapshot.key.find(':');
            const auto logical_path = separator == std::string::npos
                                          ? std::string_view{snapshot.key}
                                          : std::string_view{snapshot.key}.substr(separator + 1U);
            auto path = axk::text::path_from_utf8(logical_path);
            const auto filename =
                path && !path->filename().empty() ? axk::text::path_to_utf8(path->filename()) : snapshot.key;
            return std::format("{}:{}:{}", snapshot.object.header.raw_type, fold_ascii(filename),
                               snapshot.object.header.name);
        }
        return snapshot.key;
    };
    std::unordered_map<std::string, std::string> current_ids_by_key;
    current_ids_by_key.reserve(current.snapshots_by_id.size());
    for (const auto &[id, snapshot] : current.snapshots_by_id)
        current_ids_by_key.emplace(stable_key(current, *snapshot), id);
    std::unordered_map<std::string, std::string> remapped;
    remapped.reserve(fresh.snapshots_by_id.size());
    for (const auto &[id, snapshot] : fresh.snapshots_by_id) {
        if (const auto found = current_ids_by_key.find(stable_key(fresh, *snapshot));
            found != current_ids_by_key.end()) {
            remapped.emplace(id, found->second);
        } else {
            remapped.emplace(id, id);
        }
    }
    const auto map_id = [&](std::string &id) {
        if (const auto found = remapped.find(id); found != remapped.end())
            id = found->second;
    };
    for (auto &object : fresh.objects)
        map_id(object.id);
    for (auto &[key, id] : fresh.native_object_ids) {
        static_cast<void>(key);
        map_id(id);
    }
    for (auto &content_item : fresh.content) {
        if (content_item.object_id)
            map_id(*content_item.object_id);
    }
    for (auto &relationship : fresh.relationships) {
        map_id(relationship.source_object_id);
        if (relationship.target_object_id)
            map_id(*relationship.target_object_id);
        for (auto &candidate : relationship.candidate_object_ids)
            map_id(candidate);
    }
    for (auto &issue : fresh.validation) {
        if (issue.object_id)
            map_id(*issue.object_id);
    }
    decltype(fresh.descriptors_by_id) descriptors;
    descriptors.reserve(fresh.descriptors_by_id.size());
    for (auto &[id, descriptor] : fresh.descriptors_by_id) {
        auto mapped = id;
        map_id(mapped);
        descriptors.emplace(std::move(mapped), std::move(descriptor));
    }
    fresh.descriptors_by_id = std::move(descriptors);
    decltype(fresh.snapshots_by_id) snapshots;
    snapshots.reserve(fresh.snapshots_by_id.size());
    for (auto &[id, snapshot] : fresh.snapshots_by_id) {
        auto mapped = id;
        map_id(mapped);
        snapshots.emplace(std::move(mapped), std::move(snapshot));
    }
    fresh.snapshots_by_id = std::move(snapshots);
    decltype(fresh.waveform_status_by_id) waveform_statuses;
    waveform_statuses.reserve(fresh.waveform_status_by_id.size());
    for (auto &[id, status] : fresh.waveform_status_by_id) {
        auto mapped = id;
        map_id(mapped);
        waveform_statuses.emplace(std::move(mapped), status);
    }
    fresh.waveform_status_by_id = std::move(waveform_statuses);
    decltype(fresh.waveform_cluster_counts_by_id) waveform_cluster_counts;
    waveform_cluster_counts.reserve(fresh.waveform_cluster_counts_by_id.size());
    for (auto &[id, count] : fresh.waveform_cluster_counts_by_id) {
        auto mapped = id;
        map_id(mapped);
        waveform_cluster_counts.emplace(std::move(mapped), count);
    }
    fresh.waveform_cluster_counts_by_id = std::move(waveform_cluster_counts);
    fresh.object_indices_by_id.clear();
    fresh.object_indices_by_type.clear();
    for (std::size_t index = 0U; index < fresh.objects.size(); ++index) {
        fresh.object_indices_by_id.emplace(fresh.objects[index].id, index);
        fresh.object_indices_by_type[fresh.objects[index].type].push_back(index);
    }
    return {};
}
