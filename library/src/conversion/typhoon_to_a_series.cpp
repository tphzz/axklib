#include "axklib/conversion/typhoon_to_a_series/profile.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "a_series_destination.hpp"
#include "axklib/formats/typhoon/document.hpp"
#include "axklib/pcm_import.hpp"
#include "typhoon_instruments.hpp"

namespace axk::conversion::typhoon_to_a_series {
namespace {
constexpr auto id = "nuedge.typhoon-to-a-series";
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}
std::string folded(std::string name) {
    for (auto &c : name)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return name;
}
Result<std::shared_ptr<const PreparedPlan>> prepare(const Request &request, const CancellationToken &token,
                                                    ProgressSink *progress) {
    const auto doc = std::dynamic_pointer_cast<const typhoon::Document>(request.source);
    if (!doc || !request.destination.reader)
        return std::unexpected(invalid("Select a native Typhoon document and an A-series image"));
    const auto &options = request.options;
    const bool subset = options.contains("sourceObjectIds");
    if (!options.is_object() || options.size() != (subset ? 2U : 1U) || !options.contains("mode") ||
        (options["mode"] != "WAVE_DATA_ONLY" && options["mode"] != "HIERARCHY"))
        return std::unexpected(invalid("Select a Typhoon conversion mode"));
    const bool hierarchy = options["mode"] == "HIERARCHY";
    auto destination = volume_destination(request.destination.selection);
    if (!destination)
        return std::unexpected(destination.error());
    std::set<std::string, std::less<>> chosen, used;
    if (subset) {
        const auto &ids = options["sourceObjectIds"];
        if (!ids.is_array() || ids.empty() || ids.size() > request.limits.maximum_actions)
            return std::unexpected(invalid("Select one or more Typhoon Waves"));
        for (const auto &key : ids) {
            if (!key.is_string())
                return std::unexpected(invalid("Invalid Typhoon Wave selection"));
            auto item = doc->find(key.get_ref<const std::string &>());
            if (!item || !(item->wave || (hierarchy && (item->type == "performance" || item->type == "voice"))) ||
                !chosen.insert(item->id).second)
                return std::unexpected(invalid("Selected Typhoon object is missing, invalid or duplicated"));
        }
    }
    const auto inventory = a_series::authoring::inventory(request.destination.reader, *destination, token);
    if (!inventory)
        return std::unexpected(inventory.error());
    for (const auto &name : inventory->wave_data_names)
        used.insert(folded(name));
    Report report;
    report.profile_id = id;
    report.fidelity = request.fidelity;
    report.executable = true;
    auto issue = [&](Impact impact, std::string code, std::string object, std::string message) {
        report.issues.push_back({"issue-" + std::to_string(report.issues.size() + 1U),
                                 std::move(code),
                                 impact,
                                 std::move(object),
                                 {},
                                 {},
                                 {},
                                 std::move(message)});
        if (impact == Impact::blocker || request.fidelity == Fidelity::strict)
            report.executable = false;
    };
    a_series::authoring::Bundle bundle;
    Waves waves;
    if (hierarchy)
        select_instruments(*doc, subset, chosen, report, issue);
    std::uint64_t audio_budget = request.limits.maximum_bytes / 8U;
    for (const auto &obj : doc->catalog().objects) {
        auto item = doc->find(obj.id);
        if (!item)
            continue;
        if (item->type == "native-file" && !subset) {
            issue(Impact::blocker, "typhoon.invalid-item", item->id,
                  "An item could not be decoded; select the Waves to import explicitly");
            continue;
        }
        if (!item->wave)
            continue;
        if (auto active = token.check(); !active)
            return std::unexpected(active.error());
        if (!hierarchy)
            report.source_objects.push_back({item->id, item->name, "wave", item->occurrence, true});
        if ((hierarchy || subset) && !chosen.contains(item->id))
            continue;
        const auto &wave = *item->wave;
        WaveformSpec spec;
        spec.name = item->name.empty() ? "Wave" : item->name;
        const auto base = spec.name;
        for (unsigned suffix = 2; !used.insert(folded(spec.name)).second; ++suffix)
            spec.name = base + " " + std::to_string(suffix);
        spec.root_key = wave.root_key.value_or(60);
        spec.fine_tune_cents = wave.detune_cents;
        const auto rate = *std::ranges::min_element(supported_sampler_sample_rates, {}, [&](auto r) {
            return r > wave.sample_rate ? r - wave.sample_rate : wave.sample_rate - r;
        });
        spec.target_sample_rate = rate;
        if (!wave.root_key)
            issue(Impact::uncertainty, "typhoon.root-key", item->id, "Wave has no root key; use MIDI note 60");
        if (rate != wave.sample_rate)
            issue(Impact::loss, "typhoon.sample-rate", item->id,
                  "Resample from " + std::to_string(wave.sample_rate) + " Hz to " + std::to_string(rate) + " Hz");
        report.actions.push_back({"wave-data", spec.name, item->id});
        report.actions.back().details = {{"Root key", std::to_string(spec.root_key)},
                                         {"Sample rate", std::to_string(rate)}};
        waves.emplace(item->id, spec);
        if (!report.executable)
            continue;
        const auto scaled = [&](std::uint64_t frame) {
            return (frame * rate + wave.sample_rate / 2U) / wave.sample_rate;
        };
        const auto bytes = (static_cast<std::uint64_t>(wave.frames) + scaled(wave.frames) + 1U) * sizeof(std::int16_t);
        if (bytes > audio_budget)
            return std::unexpected(invalid("Typhoon PCM exceeds the preparation memory limit"));
        audio_budget -= bytes;
        auto pcm = doc->pcm(item->id, 0, wave.frames, token);
        if (!pcm)
            return std::unexpected(pcm.error());
        auto audio = import_mono_pcm16(*pcm, wave.sample_rate, rate);
        if (!audio)
            return std::unexpected(audio.error());
        if (wave.loop) {
            const auto first = scaled(wave.loop->first);
            const auto end = std::min<std::uint64_t>(scaled(wave.loop->second + 1U), audio->output_frames);
            if (first >= end || end > UINT32_MAX) {
                issue(Impact::blocker, "typhoon.loop", item->id,
                      "The loop cannot be represented at the target sample rate");
                continue;
            }
            spec.loop_mode = AudioSamplerLoopMode::forward_loop;
            spec.loop_start_frame = static_cast<std::uint32_t>(first);
            spec.loop_length_frames = static_cast<std::uint32_t>(end - first);
        }
        waves.at(item->id) = spec;
        bundle.wave_data.push_back({std::move(spec), std::move(*audio)});
    }
    if (hierarchy)
        map_instruments(*doc, chosen, waves, *inventory, bundle, report, issue);
    if (report.actions.empty())
        issue(Impact::blocker, "conversion.empty", {}, "No Typhoon Waves selected");
    const auto retained = report_bytes(report);
    if (report.actions.size() > request.limits.maximum_actions ||
        report.issues.size() > request.limits.maximum_issues || retained > request.limits.maximum_bytes)
        return std::unexpected(invalid("Typhoon conversion exceeds the review limit"));
    if (!report.executable)
        return std::make_shared<const PreparedPlan>(std::move(report), std::vector<Patch>{},
                                                    request.destination.reader->size(), retained);
    return finish_volume_plan(request, *destination, bundle, std::move(report), token, progress);
}
} // namespace
Profile profile() {
    return {id,
            "Typhoon to A-series",
            "nuedge.typhoon",
            "yamaha.a-series",
            {{"type", "object"},
             {"additionalProperties", false},
             {"required", {"mode"}},
             {"properties",
              {{"mode", {{"type", "string"}, {"enum", {"WAVE_DATA_ONLY", "HIERARCHY"}}}},
               {"sourceObjectIds",
                {{"type", "array"}, {"minItems", 1}, {"uniqueItems", true}, {"items", {{"type", "string"}}}}}}}},
            prepare};
}
} // namespace axk::conversion::typhoon_to_a_series
