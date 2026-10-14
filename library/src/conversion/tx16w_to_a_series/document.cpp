#include "axklib/conversion/tx16w_to_a_series/source.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <variant>

namespace axk::tx16w {

Result<Inspection> inspect_document(const Document &document, ImportMode mode, const CancellationToken &cancellation) {
    if (const auto verified = document.sources()->verify(cancellation); !verified)
        return std::unexpected(verified.error());
    Inspection result;
    std::map<std::string, std::size_t, std::less<>> waves;
    std::map<std::string, std::size_t, std::less<>> setups;
    for (const auto &object : document.native_objects()) {
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
        if (relationship.resolution != native::Resolution::resolved || !relationship.target_id) {
            result.notices.push_back({ParseNoticeDisposition::blocked, relationship.source_id, relationship.type,
                                      "Native reference is missing or ambiguous: " + relationship.native_reference});
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
