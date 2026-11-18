#include "axklib/conversion/tx16w_to_a_series/source.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <variant>

#include "axklib/formats/tx16w/dependencies.hpp"

namespace axk::tx16w {
namespace {
std::string wave_blocker(const NativeObject &object, const NativeSetup *setup,
                         const std::map<std::string, native::DependencyState> &requirements) {
    const auto &timbre = std::get<Timbre>(object.value);
    std::string message = "Timbre " + std::to_string(timbre.slot + 1U);
    if (!timbre.name.empty())
        message += " (" + timbre.name + ")";
    message += " needs Wave slot " + std::to_string(timbre.wave_slot + 1U) + ": ";
    if (!setup)
        return message + "no unique decoded Setup";
    const auto wave = std::ranges::find(setup->waves, timbre.wave_slot, &WaveReference::slot);
    if (wave == setup->waves.end())
        return message + "the Setup slot has no usable name and is not a confirmed empty slot";
    const auto filename = detail::reference_filename(wave->name, 'W', wave->slot);
    const auto it = requirements.find(filename);
    if (it != requirements.end()) {
        switch (it->second) {
        case native::DependencyState::ambiguous:
            return message + "several sources match " + filename;
        case native::DependencyState::unsupported:
            return message + filename + " is unavailable; similarly named unsupported audio is not a substitute";
        case native::DependencyState::invalid:
            return message + filename + " is invalid";
        case native::DependencyState::missing:
            return message + filename + " is missing";
        }
    }
    return message + filename + " has no unique decoded Wave";
}
} // namespace

Result<Inspection> inspect_document(const Document &document, ImportMode mode, const CancellationToken &cancellation,
                                    const std::set<std::string> *included) {
    if (const auto verified = document.sources()->verify(cancellation); !verified)
        return std::unexpected(verified.error());
    Inspection result;
    std::map<std::string, std::size_t, std::less<>> waves;
    std::map<std::string, std::size_t, std::less<>> setups;
    std::map<std::string, const NativeSetup *> setup_by_group;
    std::map<std::string, native::DependencyState> requirements;
    for (const auto &requirement : document.summary().source_requirements)
        requirements.emplace(requirement.name, requirement.state);
    for (const auto &object : document.native_objects()) {
        if (const auto *setup = std::get_if<NativeSetup>(&object.value)) {
            const auto [it, inserted] = setup_by_group.emplace(object.file.set_key, setup);
            if (!inserted)
                it->second = nullptr;
        }
        if (included && !included->contains(object.id))
            continue;
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        if (const auto *wave = std::get_if<WaveHeader>(&object.value)) {
            waves.emplace(object.id, result.waves.size());
            result.waves.push_back({*wave, {}, object.location.member_id});
            result.wave_object_ids.push_back(object.id);
        } else if (const auto *setup = std::get_if<NativeSetup>(&object.value)) {
            setups.emplace(object.id, result.setups.size());
            result.setups.push_back(*setup);
            result.resolved_setup_waves.emplace_back();
            result.setup_object_ids.push_back({object.id, {}, {}, {}});
        } else if (std::holds_alternative<OpaqueFile>(object.value)) {
            const auto summary = std::ranges::find(document.catalog().objects, object.id, &native::ObjectSummary::id);
            if (summary != document.catalog().objects.end())
                result.unsupported_files.push_back(summary->name);
        }
    }
    if (!result.unsupported_files.empty())
        result.profile = Profile::yamaha_native_with_auxiliary_files;
    for (const auto &diagnostic : document.catalog().diagnostics) {
        // Report unresolved edges once per native relationship in scope, below.
        if (diagnostic.code == "tx16w.unresolved-reference")
            continue;
        if (mode != ImportMode::wave_data_only && included && diagnostic.object_id &&
            (diagnostic.code == "tx16w.ambiguous-dependency" || diagnostic.code == "tx16w.unsupported-dependency"))
            continue; // Setup-wide Wave inventory; reachable Timbres report the precise requirement below.
        if (included && diagnostic.object_id && diagnostic.severity != native::Severity::error &&
            !included->contains(*diagnostic.object_id))
            continue;
        if (mode == ImportMode::wave_data_only && diagnostic.code != "tx16w.unknown-sample-rate" &&
            diagnostic.code != "tx16w.invalid-record" && diagnostic.code != "tx16w.unsupported-record")
            continue;
        const auto disposition = diagnostic.code == "tx16w.unknown-sample-rate"  ? ParseNoticeDisposition::defaulted
                                 : diagnostic.code == "tx16w.unsupported-record" ? ParseNoticeDisposition::omitted
                                                                                 : ParseNoticeDisposition::blocked;
        result.notices.push_back({disposition, diagnostic.object_id.value_or(""), diagnostic.code, diagnostic.message});
    }
    if (mode == ImportMode::wave_data_only)
        return result;

    // Companion membership and Wave identity come only from the native graph.
    for (const auto &relationship : document.catalog().relationships) {
        // A Setup inventories every Wave slot. Only the selected Timbres' wave edges are requirements.
        if (included && (!included->contains(relationship.source_id) ||
                         (relationship.type == "wave-reference" &&
                          (!relationship.target_id || !included->contains(*relationship.target_id)))))
            continue;
        if (relationship.resolution != native::Resolution::resolved || !relationship.target_id) {
            std::string message = "Unresolved " + relationship.type + " reference " + relationship.native_reference;
            if (relationship.type == "timbre-wave") {
                const auto source = document.object(relationship.source_id);
                if (!source)
                    return std::unexpected(source.error());
                const auto setup = setup_by_group.find((*source)->file.set_key);
                if (setup != setup_by_group.end() && setup->second &&
                    setup->second->empty_wave_slots.test(std::get<Timbre>((*source)->value).wave_slot))
                    continue; // The planner reports the affected Voice regions, not a missing file.
                message = wave_blocker(**source, setup == setup_by_group.end() ? nullptr : setup->second, requirements);
            }
            result.notices.push_back(
                {ParseNoticeDisposition::blocked, relationship.source_id, relationship.type, std::move(message)});
            continue;
        }
        const auto source = document.object(relationship.source_id);
        if (!source)
            return std::unexpected(source.error());
        if (relationship.type == "companion-setup") {
            const auto owner = setups.find(*relationship.target_id);
            if (owner == setups.end())
                continue;
            auto &setup = result.setups[owner->second];
            auto &ids = result.setup_object_ids[owner->second];
            if (const auto *voice = std::get_if<Voice>(&(*source)->value)) {
                setup.voices.push_back(*voice);
                ids.voices.emplace(voice->slot, (*source)->id);
            } else if (const auto *timbre = std::get_if<Timbre>(&(*source)->value)) {
                setup.timbres.push_back(*timbre);
                ids.timbres.emplace(timbre->slot, (*source)->id);
            } else if (const auto *performance = std::get_if<Performance>(&(*source)->value)) {
                setup.performances.push_back(*performance);
                ids.performances.emplace(performance->slot, (*source)->id);
            }
        } else if (relationship.type == "wave-reference") {
            const auto owner = setups.find(relationship.source_id);
            const auto wave = waves.find(*relationship.target_id);
            if (owner == setups.end() || wave == waves.end())
                continue;
            for (const auto &reference : result.setups[owner->second].waves)
                if (std::to_string(reference.slot) == relationship.native_reference)
                    result.resolved_setup_waves[owner->second].emplace(reference.slot, wave->second);
        }
    }
    return result;
}

} // namespace axk::tx16w
