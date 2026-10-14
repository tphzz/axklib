#include "axklib/formats/tx16w/relationships.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace axk::tx16w::detail {
namespace {

using Slot = std::pair<std::string, std::uint8_t>;
using Ids = std::vector<std::string>;
using Fileslot = std::tuple<std::string, std::string, std::string, std::uint8_t>;

std::string symbol(std::string_view name, bool filename) {
    if (filename) {
        const auto slash = name.find_last_of("/\\");
        name.remove_prefix(slash == std::string_view::npos ? 0U : slash + 1U);
        name = name.substr(0U, name.find_last_of('.'));
    }
    std::string result{name};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return character == '_' ? ' ' : static_cast<char>(std::toupper(character));
    });
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

Fileslot file_slot(const NativeObject &object, std::uint8_t slot) {
    return {object.location.member_id, object.location.root_id, object.location.entry_id, slot};
}

template <typename Key> const Ids &lookup(const std::map<Key, Ids> &index, const Key &key) {
    static const Ids empty;
    const auto found = index.find(key);
    return found == index.end() ? empty : found->second;
}

} // namespace

Result<void> build_relationships(std::span<const NativeObject> objects, std::span<const NativeFile> files,
                                 native::Catalog &catalog, const CancellationToken &cancellation) {
    std::map<Slot, Ids> waves;
    std::map<Slot, Ids> voices;
    std::map<Fileslot, Ids> timbres;
    std::map<std::string, std::vector<const NativeObject *>> setups;
    std::map<std::string, std::size_t> voice_groups;
    for (const auto &file : files)
        if (file.decoded && file.identity.kind == 'V')
            ++voice_groups[file.identity.set_key];
    for (const auto &object : objects) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        if (const auto *wave = std::get_if<WaveHeader>(&object.value)) {
            if (wave->native_slot)
                waves[{symbol(wave->name, true), *wave->native_slot}].push_back(object.id);
        } else if (const auto *voice = std::get_if<Voice>(&object.value)) {
            voices[{object.file.set_key, voice->slot}].push_back(object.id);
        } else if (const auto *timbre = std::get_if<Timbre>(&object.value)) {
            timbres[file_slot(object, timbre->slot)].push_back(object.id);
        } else if (std::holds_alternative<NativeSetup>(object.value))
            setups[object.file.set_key].push_back(&object);
    }
    std::size_t candidate_count{};
    auto link = [&](const NativeObject &source, std::string type, std::string reference, const Ids &candidates,
                    bool ambiguous_context = false) -> Result<void> {
        if (catalog.relationships.size() >= 65536U || candidates.size() > 131072U - candidate_count)
            return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                              "TX16W native relationship graph exceeds its bounded size"));
        candidate_count += candidates.size();
        native::Relationship relationship{source.id + ":" + type + ":" + reference,
                                          source.id,
                                          std::nullopt,
                                          {},
                                          std::move(type),
                                          std::move(reference),
                                          native::Resolution::missing};
        if (candidates.size() == 1U && !ambiguous_context) {
            relationship.target_id = candidates.front();
            relationship.resolution = native::Resolution::resolved;
        } else if (!candidates.empty() || ambiguous_context) {
            relationship.candidates = candidates;
            relationship.resolution = native::Resolution::ambiguous;
        }
        catalog.relationships.push_back(std::move(relationship));
        return {};
    };
    for (const auto &file : files) {
        if (!file.decoded || (file.identity.kind != 'V' && file.identity.kind != 'U'))
            continue;
        const auto owners = setups.find(file.identity.set_key);
        if (owners == setups.end())
            catalog.diagnostics.push_back({"tx16w.missing-setup", native::Severity::warning,
                                           "Native file has no decoded companion Setup: " + file.identity.set_name,
                                           std::nullopt});
        else if (owners->second.size() > 1U)
            catalog.diagnostics.push_back(
                {"tx16w.ambiguous-setup", native::Severity::warning,
                 "Native file has several possible companion Setups: " + file.identity.set_name, std::nullopt});
    }
    for (const auto &object : objects) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        if (std::holds_alternative<Voice>(object.value) || std::holds_alternative<Timbre>(object.value) ||
            std::holds_alternative<Performance>(object.value)) {
            Ids owners;
            if (const auto group = setups.find(object.file.set_key); group != setups.end())
                for (const auto *owner : group->second)
                    owners.push_back(owner->id);
            if (auto added = link(object, "companion-setup", object.file.set_key, owners); !added)
                return added;
        }
        if (const auto *setup = std::get_if<NativeSetup>(&object.value)) {
            if (!voice_groups.contains(object.file.set_key))
                catalog.diagnostics.push_back({"tx16w.missing-voices", native::Severity::warning,
                                               "Setup has no decoded companion Voice/Timbre records", object.id});
            else if (voice_groups.at(object.file.set_key) > 1U)
                catalog.diagnostics.push_back({"tx16w.ambiguous-voices", native::Severity::warning,
                                               "Setup has several possible companion Voice/Timbre files", object.id});
            for (const auto &wave : setup->waves) {
                if (auto added = link(object, "wave-reference", std::to_string(wave.slot),
                                      lookup(waves, Slot{symbol(wave.name, false), wave.slot}));
                    !added)
                    return added;
            }
        } else if (const auto *voice = std::get_if<Voice>(&object.value)) {
            for (std::size_t index = 0U; index < voice->regions.size(); ++index) {
                if (auto added = link(object, "region-timbre", std::to_string(index),
                                      lookup(timbres, file_slot(object, voice->regions[index].timbre_slot)));
                    !added)
                    return added;
            }
        } else if (const auto *timbre = std::get_if<Timbre>(&object.value)) {
            Ids candidates;
            const auto group = setups.find(object.file.set_key);
            if (group != setups.end()) {
                for (const auto *setup_object : group->second) {
                    const auto &owner_setup = std::get<NativeSetup>(setup_object->value);
                    const auto reference =
                        std::ranges::find(owner_setup.waves, timbre->wave_slot, &WaveReference::slot);
                    if (reference != owner_setup.waves.end()) {
                        const auto &matches = lookup(waves, Slot{symbol(reference->name, false), reference->slot});
                        if (matches.size() > 131072U - candidates.size())
                            return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                                              "TX16W Wave resolution exceeds its candidate limit"));
                        candidates.insert(candidates.end(), matches.begin(), matches.end());
                    }
                }
            }
            std::ranges::sort(candidates);
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            if (auto added = link(object, "timbre-wave", std::to_string(timbre->wave_slot), candidates,
                                  group != setups.end() && group->second.size() > 1U);
                !added)
                return added;
        } else if (const auto *performance = std::get_if<Performance>(&object.value)) {
            for (std::size_t index = 0U; index < performance->voices.size(); ++index) {
                const auto &assignment = performance->voices[index];
                if (assignment.voice_slot == 0xffU)
                    continue;
                if (auto added =
                        link(object, "performance-voice", std::to_string(index),
                             lookup(voices, Slot{object.file.set_key, assignment.voice_slot}),
                             voice_groups.contains(object.file.set_key) && voice_groups.at(object.file.set_key) > 1U);
                    !added)
                    return added;
            }
        }
    }
    return {};
}

} // namespace axk::tx16w::detail
