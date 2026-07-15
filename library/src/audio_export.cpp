#include "axklib/audio_export.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>

#include "axklib/media.hpp"
#include "axklib/utf8.hpp"
#include "axklib/wav_stream.hpp"

namespace axk {
namespace {

std::string safe_component(std::string value, std::string_view fallback) {
    const auto trim = [](std::string &text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
            text.erase(text.begin());
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
            text.pop_back();
        }
    };
    trim(value);
    std::size_t duplicate_count{};
    while (!value.empty() && value.back() == '*') {
        ++duplicate_count;
        value.pop_back();
    }
    if (duplicate_count != 0U)
        trim(value);
    std::string cleaned;
    bool previous_space{};
    bool previous_underscore{};
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '<' || character == '>') {
            cleaned += character == '<' ? "_lt_" : "_gt_";
            previous_space = false;
            previous_underscore = true;
            continue;
        }
        const bool invalid = byte < 0x20U || std::string_view{":\"/\\|?*"}.contains(character);
        if (invalid || character == '_') {
            if (!previous_underscore)
                cleaned += '_';
            previous_space = false;
            previous_underscore = true;
            continue;
        }
        if (std::isspace(byte) != 0) {
            if (!previous_space && !cleaned.empty())
                cleaned += ' ';
            previous_space = true;
            previous_underscore = false;
            continue;
        }
        cleaned += character;
        previous_space = false;
        previous_underscore = false;
    }
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.' || cleaned.back() == '_')) {
        cleaned.pop_back();
    }
    while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '.' || cleaned.front() == '_')) {
        cleaned.erase(cleaned.begin());
    }
    if (cleaned.empty())
        cleaned = fallback;
    if (duplicate_count != 0U)
        cleaned += std::format(" ({})", duplicate_count + 1U);
    return cleaned;
}

std::string underscore_name(std::string value, std::string_view fallback) {
    value = safe_component(std::move(value), fallback);
    for (auto &character : value) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
            character = '_';
    }
    return value;
}

std::string display_text(std::string value, std::string_view fallback) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        value.pop_back();
    return value.empty() ? std::string{fallback} : value;
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

std::optional<std::int64_t> numeric(const CurrentSbnk &bank, std::string_view name) {
    const auto *field = bank.find_numeric_field(name);
    return field == nullptr ? std::nullopt : field->value;
}

const ObjectSnapshot *object(const ObjectCatalog &catalog, std::string_view key) {
    const auto found = std::ranges::find(catalog.objects, key, &ObjectSnapshot::key);
    return found == catalog.objects.end() ? nullptr : &*found;
}

std::string sfz_region(const SampleBankExport &bank, const PhysicalWaveformExport &waveform, std::string_view role,
                       std::string sample_path, std::optional<int> pan) {
    const auto *member = &bank.decoded.left;
    if (role == "right" && bank.decoded.right)
        member = &*bank.decoded.right;
    std::string line{"<region>"};
    const auto key_low = bank.key_low == 255U ? member->root_key : bank.key_low;
    const auto key_high = bank.key_high == 128U ? member->root_key : bank.key_high;
    line += std::format(" lokey={} hikey={}", key_low, key_high);
    line += std::format(" pitch_keycenter={}", member->root_key);
    if (bank.coarse_tune >= -64 && bank.coarse_tune <= 64)
        line += std::format(" transpose={}", bank.coarse_tune);
    line += std::format(" tune={}", member->fine_tune_cents);
    if (pan)
        line += std::format(" pan={}", *pan);
    auto loop_label = waveform.waveform.loop_mode_label;
    std::ranges::transform(loop_label, loop_label.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (loop_label.contains("one") && loop_label.contains("shot")) {
        line += " loop_mode=one_shot";
    } else if (!loop_label.empty() && waveform.waveform.loop_length != 0U) {
        const auto loop_end =
            static_cast<std::uint64_t>(waveform.waveform.loop_start) + waveform.waveform.loop_length - 1U;
        line +=
            std::format(" loop_mode=loop_continuous loop_start={} loop_end={}", waveform.waveform.loop_start, loop_end);
    }
    line += " sample=" + std::move(sample_path);
    return line;
}

Result<void> write_text_atomic(const std::filesystem::path &path, std::string_view text, bool overwrite) {
    if (!overwrite && std::filesystem::exists(path)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "refusing to replace an existing SFZ")};
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create SFZ output directory")};
    }
    const auto temporary = text::temporary_sibling(path);
    if (!temporary)
        return std::unexpected{temporary.error()};
    {
        std::ofstream output{*temporary, std::ios::binary | std::ios::trunc};
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) {
            std::filesystem::remove(*temporary, error);
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary SFZ")};
        }
    }
    if (overwrite)
        std::filesystem::remove(path, error);
    std::filesystem::rename(*temporary, path, error);
    if (error) {
        std::filesystem::remove(*temporary, error);
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not publish SFZ atomically")};
    }
    return {};
}

using VolumeKey = std::pair<std::uint8_t, std::uint32_t>;

VolumeKey object_volume_key(const ObjectSnapshot &item) {
    return {item.partition.value, item.placement->volume_directory.value};
}

void populate_logical_exports(const ObjectCatalog &catalog, const RelationshipGraph &graph,
                              std::map<VolumeKey, VolumeExport> &volumes,
                              const std::unordered_map<std::string, PhysicalWaveformExport *> &physical_by_key,
                              const std::unordered_map<std::string, VolumeKey> &waveform_volume_keys,
                              std::map<VolumeKey, std::set<std::string>> &rendered_names) {
    std::map<std::string, std::set<VolumeKey>> bank_volumes;
    std::map<std::string, bool> bank_has_resolved_member;
    std::map<std::string, bool> bank_has_known_member;
    std::map<std::string, bool> bank_has_tentative_member;
    for (const auto &item : catalog.objects) {
        if (!item.placement)
            continue;
        const auto *bank = std::get_if<CurrentSbnk>(&item.object.payload);
        if (bank == nullptr)
            continue;

        SampleBankExport base;
        base.object_key = item.key;
        base.display_name = item.object.header.name;
        base.key_low = bank->key_range_low;
        base.key_high = bank->key_range_high;
        base.coarse_tune = static_cast<std::int8_t>(numeric(*bank, "coarse_tune_0x0d5").value_or(0));
        base.decoded = *bank;
        for (const auto *relation : graph.children(item.key)) {
            if ((relation->type == "SBNK_LEFT_MEMBER_TO_SMPL" || relation->type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
                relation->quality == RelationshipQuality::tentative) {
                bank_has_tentative_member[item.key] = true;
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
                bank_volumes[item.key].insert(waveform_volume_keys.at(*relation->target_key));
                bank_has_resolved_member[item.key] = true;
                if (relation->quality == RelationshipQuality::known)
                    bank_has_known_member[item.key] = true;
            }
        };
        add_member("SBNK_LEFT_MEMBER_TO_SMPL", "left");
        add_member("SBNK_RIGHT_MEMBER_TO_SMPL", "right");
        if (bank_volumes[item.key].empty())
            bank_volumes[item.key].insert(object_volume_key(item));

        for (const auto &destination : bank_volumes[item.key]) {
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
                                                 *bank});
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
            volumes.at(destination).sample_banks.push_back(std::move(output));
        }
    }

    std::map<std::string, std::set<VolumeKey>> group_volumes;
    for (const auto &item : catalog.objects) {
        if (!item.placement || !std::holds_alternative<CurrentSbac>(item.object.payload))
            continue;
        std::map<VolumeKey, std::vector<std::string>> members_by_volume;
        std::map<VolumeKey, std::vector<std::string>> relationships_by_volume;
        for (const auto *relation : graph.children(item.key)) {
            if (relation->type != "SBAC_SLOT_TO_SBNK" || !relation->target_key ||
                (relation->quality != RelationshipQuality::known && relation->quality != RelationshipQuality::likely) ||
                (!bank_has_resolved_member[*relation->target_key] &&
                 !bank_has_tentative_member[*relation->target_key])) {
                continue;
            }
            for (const auto &destination : bank_volumes[*relation->target_key]) {
                relationships_by_volume[destination].push_back(*relation->target_key);
                auto &members = members_by_volume[destination];
                if (relation->quality == RelationshipQuality::known && bank_has_known_member[*relation->target_key])
                    members.push_back(*relation->target_key);
            }
        }
        if (members_by_volume.empty())
            members_by_volume[object_volume_key(item)] = {};
        for (auto &[destination, members] : members_by_volume) {
            group_volumes[item.key].insert(destination);
            volumes.at(destination)
                .sample_bank_groups.push_back({item.key, item.object.header.name, std::move(members),
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
                                           ? group_volumes[*relation->target_key]
                                           : bank_volumes[*relation->target_key];
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
    populate_logical_exports(catalog, graph, volumes, physical_by_key, waveform_volume_keys, rendered_names);
    for (auto &[key, volume] : volumes) {
        static_cast<void>(key);
        result.volumes.push_back(std::move(volume));
    }
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
    populate_logical_exports(catalog, graph, volumes, physical_by_key, waveform_volume_keys, rendered_names);
    for (auto &[key, volume] : volumes) {
        static_cast<void>(key);
        result.volumes.push_back(std::move(volume));
    }
    return result;
}

Result<ExportResult> write_export_audio(const ExportPlan &plan, const std::filesystem::path &output_directory,
                                        bool overwrite, const CancellationToken &cancellation) {
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
            const auto path = (output_directory / volume.relative_root / waveform.relative_wav_path).lexically_normal();
            if (auto registered = register_target(path, audio_internal::WavSource::from_physical(waveform.waveform));
                !registered)
                return std::unexpected{registered.error()};
        }
        for (const auto &bank : volume.sample_banks) {
            if (!bank.rendered_wav_path || bank.members.size() != 2U)
                continue;
            const auto left =
                std::ranges::find(volume.waveforms, bank.members[0].waveform_key, &PhysicalWaveformExport::object_key);
            const auto right =
                std::ranges::find(volume.waveforms, bank.members[1].waveform_key, &PhysicalWaveformExport::object_key);
            if (left == volume.waveforms.end() || right == volume.waveforms.end())
                continue;
            const auto path = (output_directory / volume.relative_root / *bank.rendered_wav_path).lexically_normal();
            if (auto registered =
                    register_target(path, audio_internal::WavSource::from_stereo(left->waveform, right->waveform));
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
        if (const auto written = audio_internal::write_wav_atomic(path, source, overwrite, cancellation); !written)
            return std::unexpected{written.error()};
        result.written_files.push_back(path);
    }
    return result;
}

Result<SfzExportResult> write_sfz(const ExportPlan &plan, const std::filesystem::path &output_directory,
                                  bool overwrite) {
    SfzExportResult result;
    std::vector<std::filesystem::path> planned_paths;
    std::set<std::filesystem::path> reserved_paths;
    const auto reserve_path = [&](const VolumeExport &volume, std::string name) {
        const auto directory = output_directory / volume.relative_root;
        const auto stem = safe_component(std::move(name), "instrument");
        auto path = directory / (stem + ".sfz");
        for (std::size_t index = 2U; reserved_paths.contains(path); ++index)
            path = directory / std::format("{} ({}).sfz", stem, index);
        reserved_paths.insert(path);
        planned_paths.push_back(std::move(path));
    };
    for (const auto &volume : plan.volumes) {
        std::set<std::string> grouped;
        for (const auto &group : volume.sample_bank_groups) {
            reserve_path(volume, "B " + display_text(group.display_name, "instrument"));
            grouped.insert(group.member_bank_keys.begin(), group.member_bank_keys.end());
        }
        for (const auto &bank : volume.sample_banks) {
            if (!grouped.contains(bank.object_key))
                reserve_path(volume, bank.display_name);
        }
    }
    if (!overwrite) {
        const auto existing =
            std::ranges::find_if(planned_paths, [](const auto &path) { return std::filesystem::exists(path); });
        if (existing != planned_paths.end()) {
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "refusing to replace an existing SFZ: " + text::path_to_utf8(*existing))};
        }
    }
    auto planned_path = planned_paths.begin();
    for (const auto &volume : plan.volumes) {
        std::set<std::string> grouped;
        const auto write_instrument = [&](std::string name,
                                          const std::vector<const SampleBankExport *> &banks) -> Result<void> {
            const auto path = *planned_path++;
            std::string text = std::format("// Generated by axklib\n// Volume: {}\n// "
                                           "Instrument: {}\n\n<group>\n",
                                           text::path_to_utf8(volume.relative_root), name);
            std::size_t region_count{};
            for (const auto *bank : banks) {
                if (bank->rendered_wav_path && !bank->members.empty()) {
                    const auto waveform = std::ranges::find(volume.waveforms, bank->members.front().waveform_key,
                                                            &PhysicalWaveformExport::object_key);
                    if (waveform != volume.waveforms.end()) {
                        text += "// " + display_text(bank->display_name, "sample") + '\n';
                        text += sfz_region(*bank, *waveform, bank->members.front().role,
                                           text::path_to_utf8(*bank->rendered_wav_path), {}) +
                                '\n';
                        ++region_count;
                    }
                } else {
                    for (const auto &member : bank->members) {
                        if (member.quality != RelationshipQuality::known)
                            continue;
                        const auto waveform = std::ranges::find(volume.waveforms, member.waveform_key,
                                                                &PhysicalWaveformExport::object_key);
                        if (waveform == volume.waveforms.end())
                            continue;
                        const bool physical_pair =
                            bank->members.size() > 1U &&
                            std::ranges::any_of(bank->members, [](const auto &item) { return item.role == "left"; }) &&
                            std::ranges::any_of(bank->members, [](const auto &item) { return item.role == "right"; });
                        std::optional<int> pan;
                        if (physical_pair && member.role == "left")
                            pan = -100;
                        else if (physical_pair && member.role == "right")
                            pan = 100;
                        text += "// " + display_text(bank->display_name, "sample") + '\n';
                        text += sfz_region(*bank, *waveform, member.role, text::path_to_utf8(member.relative_wav_path),
                                           pan) +
                                '\n';
                        ++region_count;
                    }
                }
            }
            if (text.empty() || text.back() != '\n')
                text += '\n';
            if (region_count == 0U)
                return {};
            if (const auto written = write_text_atomic(path, text, overwrite); !written) {
                return std::unexpected{written.error()};
            }
            result.written_files.push_back(path);
            return {};
        };
        for (const auto &group : volume.sample_bank_groups) {
            std::vector<const SampleBankExport *> banks;
            for (const auto &key : group.member_bank_keys) {
                const auto found = std::ranges::find(volume.sample_banks, key, &SampleBankExport::object_key);
                if (found != volume.sample_banks.end()) {
                    banks.push_back(&*found);
                    grouped.insert(key);
                }
            }
            if (const auto written = write_instrument("B " + display_text(group.display_name, "instrument"), banks);
                !written) {
                return std::unexpected{written.error()};
            }
        }
        for (const auto &bank : volume.sample_banks) {
            if (grouped.contains(bank.object_key))
                continue;
            if (const auto written = write_instrument(bank.display_name, {&bank}); !written) {
                return std::unexpected{written.error()};
            }
        }
    }
    return result;
}

} // namespace axk
