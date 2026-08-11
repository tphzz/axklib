#include "axklib/audio_export.hpp"

#include "audio_export_support.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>

#include "axklib/export_paths.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"
#include "axklib/wav_stream.hpp"

namespace axk {
namespace {

using audio_export_detail::append_publication_warnings;
using audio_export_detail::safe_component;

std::string underscore_name(std::string value, std::string_view fallback) {
    value = safe_component(std::move(value), fallback);
    for (auto &character : value) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
            character = '_';
    }
    return value;
}

std::string unique_wav_name(std::string stem, std::set<std::string> &used) {
    stem = safe_component(std::move(stem), "waveform");
    auto candidate = stem + ".wav";
    for (std::size_t index = 2; used.contains(candidate); ++index) {
        candidate = std::format("{} ({}).wav", stem, index);
    }
    used.insert(candidate);
    return candidate;
}

std::optional<std::int64_t> numeric(const CurrentSbnk &sample, std::string_view name) {
    const auto *field = sample.find_numeric_field(name);
    return field == nullptr ? std::nullopt : field->value;
}

const ObjectSnapshot *object(const ObjectCatalog &catalog, std::string_view key) {
    const auto found = std::ranges::find(catalog.objects, key, &ObjectSnapshot::key);
    return found == catalog.objects.end() ? nullptr : &*found;
}

using VolumeKey = std::pair<std::uint8_t, std::uint32_t>;

VolumeKey object_volume_key(const ObjectSnapshot &item) {
    return {item.partition.value, item.placement->volume_directory.value};
}

std::string unresolved_partition_name(const ObjectSnapshot &item, std::string fallback) {
    if (!item.placement_candidates.empty() && !item.placement_candidates.front().partition_name.empty())
        return item.placement_candidates.front().partition_name;
    return fallback.empty() ? std::string{"partition"} : fallback;
}

UnresolvedWaveDataExport &unresolved_scope(ExportPlan &plan, PartitionIndex partition, std::string partition_name) {
    const auto found = std::ranges::find(plan.unresolved_wave_data, partition, &UnresolvedWaveDataExport::partition);
    if (found != plan.unresolved_wave_data.end())
        return *found;
    UnresolvedWaveDataExport scope;
    scope.partition = partition;
    scope.partition_name = std::move(partition_name);
    scope.relative_root = std::filesystem::path{std::format("partition_{:02}_{}", partition.value,
                                                            underscore_name(scope.partition_name, "partition"))} /
                          "Unresolved Wave Data";
    plan.unresolved_wave_data.push_back(std::move(scope));
    return plan.unresolved_wave_data.back();
}

void populate_logical_exports(const ObjectCatalog &catalog, const RelationshipGraph &graph,
                              std::map<VolumeKey, VolumeExport> &volumes,
                              const std::unordered_map<std::string, PhysicalWaveformExport *> &physical_by_key,
                              const std::unordered_map<std::string, VolumeKey> &waveform_volume_keys,
                              std::map<VolumeKey, std::set<std::string>> &rendered_names) {
    std::map<std::string, std::set<VolumeKey>> sample_volumes;
    std::map<std::string, bool> sample_has_resolved_member;
    std::map<std::string, bool> sample_has_known_member;
    std::map<std::string, bool> sample_has_tentative_member;
    for (const auto &item : catalog.objects) {
        if (!item.placement)
            continue;
        const auto *sample = std::get_if<CurrentSbnk>(&item.object.payload);
        if (sample == nullptr)
            continue;

        SampleExport base;
        base.object_key = item.key;
        base.display_name = item.object.header.name;
        base.key_low = sample->key_range_low;
        base.key_high = sample->key_range_high;
        base.coarse_tune = static_cast<std::int8_t>(numeric(*sample, "coarse_tune_0x0d5").value_or(0));
        base.decoded = *sample;
        for (const auto *relation : graph.children(item.key)) {
            if ((relation->type == "SBNK_LEFT_MEMBER_TO_SMPL" || relation->type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
                relation->quality == RelationshipQuality::tentative) {
                sample_has_tentative_member[item.key] = true;
            }
        }
        const auto add_member = [&](std::string_view type, std::string role) {
            for (const auto *relation : graph.children(item.key)) {
                if (relation->type != type ||
                    (relation->quality != RelationshipQuality::known &&
                     relation->quality != RelationshipQuality::likely) ||
                    !relation->target_key || !physical_by_key.contains(*relation->target_key)) {
                    continue;
                }
                const auto *physical = physical_by_key.at(*relation->target_key);
                base.members.push_back({role, *relation->target_key, physical->relative_wav_path, relation->quality});
                sample_volumes[item.key].insert(waveform_volume_keys.at(*relation->target_key));
                sample_has_resolved_member[item.key] = true;
                if (relation->quality == RelationshipQuality::known)
                    sample_has_known_member[item.key] = true;
            }
        };
        add_member("SBNK_LEFT_MEMBER_TO_SMPL", "left");
        add_member("SBNK_RIGHT_MEMBER_TO_SMPL", "right");
        if (sample_volumes[item.key].empty())
            sample_volumes[item.key].insert(object_volume_key(item));

        for (const auto &destination : sample_volumes[item.key]) {
            auto output = base;
            std::erase_if(output.members, [&](const auto &member) {
                return waveform_volume_keys.at(member.waveform_key) != destination;
            });
            if (output.members.size() == 2U && std::ranges::all_of(output.members, [](const auto &member) {
                    return member.quality == RelationshipQuality::known;
                })) {
                const auto *left = physical_by_key.at(output.members[0].waveform_key);
                const auto *right = physical_by_key.at(output.members[1].waveform_key);
                output.stereo_decision = stereo_render_decision(left->waveform, right->waveform);
                if (output.stereo_decision->renderable) {
                    output.rendered_wav_path = std::filesystem::path{"RENDERED"} /
                                               unique_wav_name(output.display_name, rendered_names[destination]);
                }
            }
            output.parameter_contexts.push_back({item.key, base.display_name,
                                                 output.members.empty() || output.members.front().role == "left"
                                                     ? "SBNK_LEFT_MEMBER_TO_SMPL"
                                                     : "SBNK_RIGHT_MEMBER_TO_SMPL",
                                                 *sample});
            std::set<std::string> context_keys{item.key};
            for (const auto &member : output.members) {
                for (const auto *relation : graph.parents(member.waveform_key)) {
                    if (!relation->target_key || *relation->target_key != member.waveform_key ||
                        relation->quality != RelationshipQuality::known ||
                        (relation->type != "SBNK_LEFT_MEMBER_TO_SMPL" &&
                         relation->type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
                        !context_keys.insert(relation->source_key).second) {
                        continue;
                    }
                    const auto *source = object(catalog, relation->source_key);
                    if (source == nullptr)
                        continue;
                    const auto *parameters = std::get_if<CurrentSbnk>(&source->object.payload);
                    if (parameters != nullptr) {
                        output.parameter_contexts.push_back(
                            {source->key, source->object.header.name, relation->type, *parameters});
                    }
                }
            }
            volumes.at(destination).samples.push_back(std::move(output));
        }
    }

    std::map<std::string, std::set<VolumeKey>> sample_bank_volumes;
    for (const auto &item : catalog.objects) {
        if (!item.placement || !std::holds_alternative<CurrentSbac>(item.object.payload))
            continue;
        std::map<VolumeKey, std::vector<std::string>> members_by_volume;
        std::map<VolumeKey, std::vector<std::string>> relationships_by_volume;
        for (const auto *relation : graph.children(item.key)) {
            if (relation->type != "SBAC_SLOT_TO_SBNK" || !relation->target_key ||
                (relation->quality != RelationshipQuality::known && relation->quality != RelationshipQuality::likely) ||
                (!sample_has_resolved_member[*relation->target_key] &&
                 !sample_has_tentative_member[*relation->target_key])) {
                continue;
            }
            for (const auto &destination : sample_volumes[*relation->target_key]) {
                relationships_by_volume[destination].push_back(*relation->target_key);
                auto &members = members_by_volume[destination];
                if (relation->quality == RelationshipQuality::known && sample_has_known_member[*relation->target_key])
                    members.push_back(*relation->target_key);
            }
        }
        if (members_by_volume.empty())
            members_by_volume[object_volume_key(item)] = {};
        for (auto &[destination, members] : members_by_volume) {
            sample_bank_volumes[item.key].insert(destination);
            volumes.at(destination)
                .sample_banks.push_back({item.key, item.object.header.name, std::move(members),
                                         std::move(relationships_by_volume[destination])});
        }
    }

    for (const auto &item : catalog.objects) {
        if (!item.placement || !std::holds_alternative<CurrentProg>(item.object.payload))
            continue;
        std::map<VolumeKey, std::vector<std::string>> targets_by_volume;
        for (const auto *relation : graph.children(item.key)) {
            if (!relation->type.starts_with("PROG_ASSIGNMENT_TO_") || !relation->target_key ||
                (relation->assignment_state != AssignmentState::active &&
                 relation->assignment_state != AssignmentState::source_load)) {
                continue;
            }
            const auto &destinations = relation->type == "PROG_ASSIGNMENT_TO_SBAC"
                                           ? sample_bank_volumes[*relation->target_key]
                                           : sample_volumes[*relation->target_key];
            for (const auto &destination : destinations)
                targets_by_volume[destination].push_back(*relation->target_key);
        }
        if (targets_by_volume.empty())
            targets_by_volume[object_volume_key(item)] = {};
        for (auto &[destination, targets] : targets_by_volume) {
            volumes.at(destination).programs.push_back({item.key, item.object.header.name, std::move(targets)});
        }
    }
}

int relationship_quality_rank(RelationshipQuality quality) {
    switch (quality) {
    case RelationshipQuality::known:
        return 0;
    case RelationshipQuality::likely:
        return 1;
    case RelationshipQuality::tentative:
        return 2;
    case RelationshipQuality::unknown:
        return 3;
    }
    return 3;
}

void populate_waveform_aliases(const ObjectCatalog &catalog, const RelationshipGraph &graph, ExportPlan &plan) {
    std::unordered_map<std::string, PhysicalWaveformExport *> waveforms;
    for (auto &volume : plan.volumes) {
        for (auto &waveform : volume.waveforms)
            waveforms.emplace(waveform.object_key, &waveform);
    }
    for (auto &scope : plan.unresolved_wave_data) {
        for (auto &waveform : scope.waveforms)
            waveforms.emplace(waveform.object_key, &waveform);
    }

    for (const auto &relation : graph.relationships) {
        if (!relation.target_key ||
            (relation.type != "SBNK_LEFT_MEMBER_TO_SMPL" && relation.type != "SBNK_RIGHT_MEMBER_TO_SMPL")) {
            continue;
        }
        const auto waveform = waveforms.find(*relation.target_key);
        const auto *sample = object(catalog, relation.source_key);
        if (waveform == waveforms.end() || sample == nullptr ||
            !std::holds_alternative<CurrentSbnk>(sample->object.payload)) {
            continue;
        }
        auto &aliases = waveform->second->user_facing_aliases;
        const auto existing = std::ranges::find(aliases, sample->key, &WaveformUserFacingAlias::sample_object_key);
        if (existing == aliases.end()) {
            aliases.push_back({sample->key, sample->object.header.name, relation.quality});
        } else if (relationship_quality_rank(relation.quality) <
                   relationship_quality_rank(existing->relationship_quality)) {
            existing->relationship_quality = relation.quality;
        }
    }
    for (auto &[key, waveform] : waveforms) {
        static_cast<void>(key);
        std::ranges::sort(waveform->user_facing_aliases, {}, &WaveformUserFacingAlias::sample_object_key);
    }
}

} // namespace

Result<ExportPlan> build_export_plan(const Container &container, const ObjectCatalog &catalog,
                                     const RelationshipGraph &graph, const CancellationToken &cancellation) {
    ExportPlan result;
    result.source_path = container.source_path();
    using Key = VolumeKey;
    std::map<Key, VolumeExport> volumes;
    for (const auto &item : catalog.objects) {
        if (!item.placement)
            continue;
        const Key key{item.partition.value, item.placement->volume_directory.value};
        auto &volume = volumes[key];
        volume.partition = item.partition;
        volume.volume_directory = item.placement->volume_directory;
        volume.partition_name = item.placement->partition_name;
        volume.volume_name = item.placement->volume_name;
        volume.relative_root =
            std::filesystem::path{std::format("partition_{:02}_{}", item.partition.value,
                                              underscore_name(item.placement->partition_name, "partition"))} /
            safe_component(item.placement->volume_name, "volume");
    }

    std::unordered_map<std::string, PhysicalWaveformExport *> physical_by_key;
    std::unordered_map<std::string, VolumeKey> waveform_volume_keys;
    std::map<Key, std::set<std::string>> rendered_names;
    for (auto &[key, volume] : volumes) {
        const auto waveform_count = std::ranges::count_if(catalog.objects, [&](const auto &item) {
            return item.placement && item.object.header.type == ObjectType::smpl &&
                   item.partition.value == volume.partition.value &&
                   item.placement->volume_directory.value == key.second;
        });
        volume.waveforms.reserve(static_cast<std::size_t>(waveform_count));
        std::set<std::string> used;
        for (const auto &item : catalog.objects) {
            if (!item.placement || item.object.header.type != ObjectType::smpl ||
                item.partition.value != volume.partition.value ||
                item.placement->volume_directory.value != key.second) {
                continue;
            }
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            auto waveform = decode_waveform(container, item, cancellation);
            if (!waveform)
                return std::unexpected{waveform.error()};
            const auto filename = unique_wav_name(item.object.header.name, used);
            volume.waveforms.push_back(
                {item.key, item.object.header.name, std::filesystem::path{"SMPL"} / filename, std::move(*waveform)});
            physical_by_key[item.key] = &volume.waveforms.back();
            waveform_volume_keys[item.key] = key;
        }
    }
    for (const auto &item : catalog.objects) {
        if (item.placement || item.object.header.type != ObjectType::smpl)
            continue;
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto partition = std::ranges::find(container.partitions(), item.partition, &Partition::index);
        const auto partition_name = unresolved_partition_name(
            item, partition == container.partitions().end() ? std::string{} : partition->name);
        auto waveform = decode_waveform(container, item, cancellation);
        if (!waveform)
            return std::unexpected{waveform.error()};
        auto &scope = unresolved_scope(result, item.partition, partition_name);
        std::set<std::string> used;
        for (const auto &existing : scope.waveforms)
            used.insert(existing.relative_wav_path.filename().string());
        scope.waveforms.push_back({item.key, item.object.header.name,
                                   std::filesystem::path{"SMPL"} / unique_wav_name(item.object.header.name, used),
                                   std::move(*waveform), item.placement_resolution, item.placement_candidates});
    }
    populate_logical_exports(catalog, graph, volumes, physical_by_key, waveform_volume_keys, rendered_names);
    for (auto &[key, volume] : volumes) {
        static_cast<void>(key);
        result.volumes.push_back(std::move(volume));
    }
    populate_waveform_aliases(catalog, graph, result);
    return result;
}

Result<ExportPlan> build_export_plan(const MediaContainer &container, const ObjectCatalog &catalog,
                                     const RelationshipGraph &graph, const CancellationToken &cancellation) {
    if (container.kind() == MediaKind::sfs) {
        return build_export_plan(std::get<Container>(container.storage()), catalog, graph, cancellation);
    }
    auto media_objects = container.objects(64U * 1024U * 1024U, cancellation);
    if (!media_objects)
        return std::unexpected{media_objects.error()};
    const auto paths = structured_object_paths(*media_objects);
    std::unordered_map<std::string, const MediaObject *> media_by_key;
    std::unordered_map<std::string, std::filesystem::path> roots_by_key;
    for (std::size_t index = 0; index < media_objects->size(); ++index) {
        const auto &item = (*media_objects)[index];
        media_by_key.emplace(item.key, &item);
        roots_by_key.emplace(item.key, paths[index].relative_path.parent_path().parent_path());
    }

    ExportPlan result;
    result.source_path = container.source_path();
    using Key = VolumeKey;
    std::map<Key, VolumeExport> volumes;
    for (const auto &item : catalog.objects) {
        if (!item.placement)
            continue;
        const Key key{item.partition.value, item.placement->volume_directory.value};
        auto &volume = volumes[key];
        volume.partition = item.partition;
        volume.volume_directory = item.placement->volume_directory;
        volume.partition_name = item.placement->partition_name;
        volume.volume_name = item.placement->volume_name;
        volume.relative_root = roots_by_key.at(item.key);
    }

    std::unordered_map<std::string, PhysicalWaveformExport *> physical_by_key;
    std::unordered_map<std::string, VolumeKey> waveform_volume_keys;
    std::map<Key, std::set<std::string>> rendered_names;
    for (auto &[key, volume] : volumes) {
        const auto count = std::ranges::count_if(catalog.objects, [&](const auto &item) {
            return item.placement && item.object.header.type == ObjectType::smpl &&
                   item.partition.value == volume.partition.value &&
                   item.placement->volume_directory.value == key.second;
        });
        volume.waveforms.reserve(static_cast<std::size_t>(count));
        std::set<std::string> used;
        for (const auto &item : catalog.objects) {
            if (!item.placement || item.object.header.type != ObjectType::smpl ||
                item.partition.value != volume.partition.value ||
                item.placement->volume_directory.value != key.second) {
                continue;
            }
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto source = media_by_key.find(item.key);
            if (source == media_by_key.end()) {
                return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                                  "media object disappeared while building export plan")};
            }
            auto waveform = decode_waveform(*source->second);
            if (!waveform) {
                result.decode_errors.push_back(
                    std::format("{} ({}): {}", item.object.header.name, item.key, waveform.error().message));
                continue;
            }
            volume.waveforms.push_back({item.key, item.object.header.name,
                                        std::filesystem::path{"SMPL"} / unique_wav_name(item.object.header.name, used),
                                        std::move(*waveform)});
            physical_by_key[item.key] = &volume.waveforms.back();
            waveform_volume_keys[item.key] = key;
        }
    }
    for (const auto &item : catalog.objects) {
        if (item.placement || item.object.header.type != ObjectType::smpl)
            continue;
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto source = media_by_key.find(item.key);
        if (source == media_by_key.end()) {
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "media object disappeared while building export plan")};
        }
        auto waveform = decode_waveform(*source->second);
        if (!waveform) {
            result.decode_errors.push_back(
                std::format("{} ({}): {}", item.object.header.name, item.key, waveform.error().message));
            continue;
        }
        auto &scope = unresolved_scope(result, item.partition, unresolved_partition_name(item, "partition"));
        std::set<std::string> used;
        for (const auto &existing : scope.waveforms)
            used.insert(existing.relative_wav_path.filename().string());
        scope.waveforms.push_back({item.key, item.object.header.name,
                                   std::filesystem::path{"SMPL"} / unique_wav_name(item.object.header.name, used),
                                   std::move(*waveform), item.placement_resolution, item.placement_candidates});
    }
    populate_logical_exports(catalog, graph, volumes, physical_by_key, waveform_volume_keys, rendered_names);
    for (auto &[key, volume] : volumes) {
        static_cast<void>(key);
        result.volumes.push_back(std::move(volume));
    }
    populate_waveform_aliases(catalog, graph, result);
    return result;
}

Result<ExportResult> write_export_audio(const ExportPlan &plan, const std::filesystem::path &output_directory,
                                        bool overwrite, const CancellationToken &cancellation) {
    if (auto valid = audio_internal::validate_export_plan_paths(plan, output_directory); !valid)
        return std::unexpected{valid.error()};
    ExportResult result;
    std::map<std::filesystem::path, audio_internal::WavSource> targets;
    const auto register_target = [&](const std::filesystem::path &path,
                                     audio_internal::WavSource source) -> Result<void> {
        const auto [existing, inserted] = targets.emplace(path, source);
        if (inserted)
            return {};
        auto equal = audio_internal::equal_wav(existing->second, source, cancellation);
        if (!equal)
            return std::unexpected{equal.error()};
        if (!*equal) {
            return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::audio,
                                              "distinct audio exports share output path: " + text::path_to_utf8(path))};
        }
        return {};
    };
    for (const auto &volume : plan.volumes) {
        for (const auto &waveform : volume.waveforms) {
            const std::array parts{volume.relative_root, waveform.relative_wav_path};
            const auto path = *audio_internal::resolve_export_destination(output_directory, parts);
            if (auto registered = register_target(path, audio_internal::WavSource::from_physical(waveform.waveform));
                !registered)
                return std::unexpected{registered.error()};
        }
        for (const auto &sample : volume.samples) {
            if (!sample.rendered_wav_path || sample.members.size() != 2U)
                continue;
            const auto left = std::ranges::find(volume.waveforms, sample.members[0].waveform_key,
                                                &PhysicalWaveformExport::object_key);
            const auto right = std::ranges::find(volume.waveforms, sample.members[1].waveform_key,
                                                 &PhysicalWaveformExport::object_key);
            if (left == volume.waveforms.end() || right == volume.waveforms.end())
                continue;
            const std::array parts{volume.relative_root, *sample.rendered_wav_path};
            const auto path = *audio_internal::resolve_export_destination(output_directory, parts);
            if (auto registered =
                    register_target(path, audio_internal::WavSource::from_stereo(left->waveform, right->waveform));
                !registered)
                return std::unexpected{registered.error()};
        }
    }
    for (const auto &scope : plan.unresolved_wave_data) {
        for (const auto &waveform : scope.waveforms) {
            const std::array parts{scope.relative_root, waveform.relative_wav_path};
            const auto path = *audio_internal::resolve_export_destination(output_directory, parts);
            if (auto registered = register_target(path, audio_internal::WavSource::from_physical(waveform.waveform));
                !registered)
                return std::unexpected{registered.error()};
        }
    }
    if (!overwrite) {
        const auto existing =
            std::ranges::find_if(targets, [](const auto &entry) { return std::filesystem::exists(entry.first); });
        if (existing != targets.end()) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                           "refusing to replace an existing audio export: " + text::path_to_utf8(existing->first))};
        }
    }
    for (const auto &[path, source] : targets) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto written = audio_internal::write_wav_atomic(path, source, overwrite, cancellation);
        if (!written)
            return std::unexpected{written.error()};
        append_publication_warnings(result.warnings, *written);
        result.written_files.push_back(path);
    }
    return result;
}

} // namespace axk
