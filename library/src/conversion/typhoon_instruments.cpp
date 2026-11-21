#include "typhoon_instruments.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <set>
#include <string_view>
#include <utility>

namespace axk::conversion::typhoon_to_a_series {
namespace {
// Closest A-series Sample Level for each Typhoon Group attenuation, from loudest to quietest.
constexpr auto sample_levels = std::to_array<std::uint8_t>({
    127, 122, 122, 117, 117, 112, 112, 105, 105, 100, 100, 96, 96, 92, 92, 90, 90, 86, 86, 83, 83, 79, 79, 74, 74, 71,
    71,  68,  68,  65,  65,  64,  64,  61,  61,  58,  58,  56, 56, 52, 52, 50, 50, 48, 48, 46, 46, 45, 45, 43, 43, 41,
    41,  39,  39,  37,  37,  36,  36,  34,  34,  33,  33,  32, 32, 30, 30, 29, 29, 28, 28, 26, 26, 25, 25, 24, 24, 23,
    23,  22,  22,  21,  21,  20,  20,  20,  20,  19,  19,  18, 18, 17, 17, 16, 16, 16, 16, 15, 15, 15, 15, 14, 14, 13,
    13,  13,  13,  12,  12,  11,  11,  11,  11,  11,  11,  10, 10, 10, 10, 9,  9,  9,  9,  9,  9,  8,  8,  2,
});
static_assert(sample_levels.size() == 128);
struct SamplePitch {
    std::int8_t coarse, fine;
    bool clipped;
};
SamplePitch sample_pitch(const typhoon::Region &region, const typhoon::Wave &wave) {
    const int pivot = 60 - wave.playback_origin.value_or(60);
    // Typhoon adds Group coarse to its origin correction in a signed byte before applying key tracking.
    const auto at_pivot = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(pivot + region.coarse_semitones));
    const int coarse = at_pivot - (region.raw_key_scaling == 0 ? 0 : pivot);
    const int fine = region.fine_steps - wave.playback_detune_steps();

    // Keep the native dial values when possible. Otherwise trade a semitone for 1024/12 fine steps.
    int target = std::clamp(coarse, -64, 63);
    const auto remainder = [&](int semitones) {
        const int thirds = (coarse - semitones) * 256;
        return fine + (thirds + (thirds < 0 ? -1 : 1)) / 3;
    };
    while (remainder(target) > 63 && target < 63)
        ++target;
    while (remainder(target) < -63 && target > -64)
        --target;
    const int residual = remainder(target);
    const int stored = std::clamp(residual, -63, 63);
    return {static_cast<std::int8_t>(target), static_cast<std::int8_t>(stored), stored != residual};
}
struct Playback {
    AudioSamplerLoopMode mode;
    std::string_view loss{};
};
Playback group_playback(typhoon::TriggerMode trigger, bool looped) {
    using enum typhoon::TriggerMode;
    const auto gated = looped ? AudioSamplerLoopMode::forward_loop : AudioSamplerLoopMode::forward;
    switch (trigger) {
    case normal:
        return {gated};
    case one_shot:
        if (looped)
            return {gated, "Looped One-shot cannot be reproduced; keeping the loop with normal note-off"};
        return {AudioSamplerLoopMode::forward_one_shot};
    case glide:
        return {gated, "Glide is not transferred; using normal note-on and note-off"};
    case release:
        return {looped ? gated : AudioSamplerLoopMode::forward_one_shot,
                "Release trigger is not transferred; the Sample will start on note-on instead of note-off"};
    }
    std::unreachable();
}
std::string unique(std::string name, std::size_t length, Ids &used) {
    if (name.empty())
        name = "TYPHOON";
    name.resize(std::min(name.size(), length));
    auto key = [](std::string s) {
        for (auto &c : s)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        return s;
    };
    if (used.insert(key(name)).second)
        return name;
    for (std::size_t n = 2;; ++n) {
        auto suffix = " " + std::to_string(n);
        auto value = name.substr(0, length - suffix.size()) + suffix;
        if (used.insert(key(value)).second)
            return value;
    }
}
Ids used_names(const std::vector<std::string> &names) {
    Ids result;
    for (auto n : names) {
        for (auto &c : n)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        result.insert(std::move(n));
    }
    return result;
}
} // namespace
void select_instruments(const typhoon::Document &doc, bool subset, Ids &chosen, Report &report, const Notice &issue) {
    const bool has_performance =
        std::ranges::any_of(doc.catalog().objects, [](const auto &o) { return o.type == "performance"; });
    const bool has_voice = std::ranges::any_of(doc.catalog().objects, [](const auto &o) { return o.type == "voice"; });
    const auto root_type = has_performance ? "performance" : has_voice ? "voice" : "wave";
    for (const auto &obj : doc.catalog().objects) {
        const auto &item = *doc.find(obj.id);
        if (item.type != "performance" && item.type != "voice" && item.type != "wave")
            continue;
        const bool root = item.type == root_type;
        SourceObject row{item.id, item.name, item.type, item.occurrence, root};
        for (const auto &r : item.references)
            if (r.target)
                row.dependencies.push_back(*r.target);
        report.source_objects.push_back(std::move(row));
        if (!subset && root)
            chosen.insert(item.id);
    }
    std::vector<std::string> work(chosen.begin(), chosen.end());
    for (std::size_t i = 0; i < work.size(); ++i) {
        const auto &item = *doc.find(work[i]);
        if (!item.mapping_error.empty()) {
            issue(Impact::blocker, "typhoon.instrument-layout", item.id, item.mapping_error);
            continue;
        }
        auto require = [&](std::size_t index) {
            const auto &r = item.references.at(index);
            if (!r.target) {
                std::string parents;
                for (const auto &row : report.source_objects) {
                    if (row.kind != "performance" || !chosen.contains(row.id) ||
                        std::ranges::find(row.dependencies, item.id) == row.dependencies.end())
                        continue;
                    parents += (parents.empty() ? " Required by: " : ", ") + row.name;
                }
                issue(Impact::blocker, "typhoon.unresolved-reference", item.id,
                      std::format("{} needs {} {} (version {:08X}){}. {}.{} Add the disk containing this version or "
                                  "remove the affected instruments from the selection.",
                                  item.name, r.type == "wave" ? "Wave" : "Voice", r.name, r.version,
                                  r.disk.empty() ? "" : " from " + r.disk,
                                  r.ambiguous ? "Multiple items match" : "This version is missing",
                                  parents.empty() ? "" : parents + "."));
            } else if (chosen.insert(*r.target).second)
                work.push_back(*r.target);
        };
        for (const auto &r : item.regions)
            require(r.reference);
        for (const auto &p : item.parts)
            require(p.reference);
    }
}
void map_instruments(const typhoon::Document &doc, const Ids &chosen, const Waves &waves,
                     const a_series::authoring::Inventory &inventory, a_series::authoring::Bundle &bundle,
                     Report &report, const Notice &issue) {
    auto samples = used_names(inventory.sample_names), banks = used_names(inventory.sample_bank_names),
         programs = used_names(inventory.program_names);
    std::map<std::string, std::string> bank_ids;
    Ids unmapped_silent_voices;
    std::set<unsigned> slots(inventory.occupied_program_slots.begin(), inventory.occupied_program_slots.end());
    for (const auto &obj : doc.catalog().objects) {
        const auto &item = *doc.find(obj.id);
        if (item.type != "voice" || !chosen.contains(item.id))
            continue;
        SampleBankSpec bank;
        bank.name = unique(item.name, 16, banks);
        std::set<std::uint8_t> reported_scalings;
        std::set<std::uint8_t> reported_volumes;
        std::set<typhoon::TriggerMode> reported_triggers;
        for (const auto &r : item.regions) {
            const auto &ref = item.references.at(r.reference);
            if (!ref.target || !waves.contains(*ref.target))
                continue;
            const auto &w = waves.at(*ref.target);
            const auto &source_wave = *doc.find(*ref.target)->wave;
            SampleSpec sample;
            sample.name = unique(ref.name, 16, samples);
            sample.waveform_id = w.name;
            sample.parameters.root_key = w.root_key;
            sample.parameters.key_low = r.midi_low;
            sample.parameters.key_high = r.midi_high;
            sample.parameters.velocity_low = r.velocity_low;
            sample.parameters.velocity_high = r.velocity_high;
            const auto level = sample_levels[r.raw_group_volume];
            sample.parameters.level = level;
            if ((r.raw_group_volume == 123 || r.raw_group_volume == 124 || r.raw_group_volume == 127) &&
                reported_volumes.insert(r.raw_group_volume).second)
                issue(Impact::loss, "typhoon.group-volume-approximation", item.id,
                      std::format("Group Volume (raw {}) maps to Sample Level {}, about {} dB louder than the source",
                                  r.raw_group_volume, level, r.raw_group_volume == 127 ? "5.9" : "1.2"));
            const bool fixed = r.raw_key_scaling == 0;
            sample.parameters.fixed_pitch = fixed;
            const auto pitch = sample_pitch(r, source_wave);
            sample.parameters.coarse_tune = pitch.coarse;
            sample.parameters.fine_tune_cents = pitch.fine;
            if (pitch.clipped)
                issue(Impact::loss, "typhoon.pitch-range", item.id,
                      std::format("Pitch for Sample {} exceeds the destination range; using Coarse Tune {} and Fine "
                                  "Tune {}",
                                  sample.name, pitch.coarse, pitch.fine));
            if (!fixed && r.raw_key_scaling != 3 && reported_scalings.insert(r.raw_key_scaling).second)
                issue(Impact::loss, "typhoon.key-scaling", item.id,
                      std::format("Group key scaling 0x{:02X} is not supported; using normal tracking",
                                  r.raw_key_scaling));
            const auto playback = group_playback(r.trigger, source_wave.loop.has_value());
            sample.parameters.loop_mode = playback.mode;
            if (!playback.loss.empty() && reported_triggers.insert(r.trigger).second)
                issue(Impact::loss, "typhoon.trigger-mode", item.id, std::string(playback.loss));
            sample.parameters.loop_start_frame = w.loop_start_frame;
            sample.parameters.loop_length_frames = w.loop_length_frames;
            bank.member_samples.push_back(sample.name);
            report.actions.push_back({"sample", sample.name, item.id});
            report.actions.back().details = {
                {"Wave Data", w.name},
                {"Key range", std::to_string(r.midi_low) + ".." + std::to_string(r.midi_high)},
                {"Velocity range", std::to_string(r.velocity_low) + ".." + std::to_string(r.velocity_high)},
                {"Root key", std::to_string(w.root_key)},
                {"Sample Level", std::to_string(level)},
                {"Fixed Pitch", fixed ? "On" : "Off"},
                {"Coarse Tune", std::to_string(pitch.coarse)},
                {"Fine Tune", std::to_string(pitch.fine)}};
            bundle.samples.push_back(std::move(sample));
        }
        if (bank.member_samples.empty()) {
            if (item.mapping_error.empty() && item.has_split_table && item.regions.empty()) {
                unmapped_silent_voices.insert(item.id);
                issue(Impact::loss, "typhoon.silent-voice", item.id,
                      "The Voice has no Wave assigned to playable keys; its empty Sample Bank is omitted");
            } else {
                issue(Impact::blocker, "typhoon.empty-voice", item.id, "Voice has no playable Wave regions");
            }
            continue;
        }
        issue(Impact::loss, "typhoon.voice-controls", item.id,
              "Group envelopes, output, filter and modulation are not transferred");
        bank_ids.emplace(item.id, bank.name);
        report.actions.push_back({"sample-bank", bank.name, item.id});
        bundle.sample_banks.push_back(std::move(bank));
    }
    for (const auto &obj : doc.catalog().objects) {
        const auto &item = *doc.find(obj.id);
        if (item.type != "performance" || !chosen.contains(item.id))
            continue;
        unsigned n = 1;
        while (n <= 128 && slots.contains(n))
            ++n;
        if (n > 128) {
            issue(Impact::blocker, "typhoon.program-capacity", item.id, "No free Program slot");
            continue;
        }
        ProgramSpec program;
        program.number = static_cast<std::uint8_t>(n);
        program.name = unique(item.name, 8, programs);
        for (const auto &p : item.parts) {
            const auto &ref = item.references.at(p.reference);
            if (!ref.target || !bank_ids.contains(*ref.target))
                continue;
            ProgramAssignmentSpec assignment;
            assignment.target_kind = "SBAC";
            assignment.target_name = bank_ids.at(*ref.target);
            if (p.channel < 16)
                assignment.parameters.receive =
                    ProgramReceiveChannel{MidiPort::a, static_cast<std::uint8_t>(p.channel + 1)};
            else if (p.channel == 255)
                issue(Impact::loss, "typhoon.any-channel", item.id,
                      "Any-channel receive is replaced by the destination default");
            else {
                issue(Impact::blocker, "typhoon.channel", item.id, "Unknown Performance receive channel");
                continue;
            }
            program.assignments.push_back(std::move(assignment));
        }
        if (program.assignments.empty()) {
            const bool only_empty = !item.parts.empty() && std::ranges::all_of(item.parts, [&](const auto &part) {
                const auto &target = item.references.at(part.reference).target;
                return target && unmapped_silent_voices.contains(*target);
            });
            if (only_empty)
                issue(Impact::loss, "typhoon.silent-performance", item.id,
                      "All assigned Voices have empty Wave maps; this Performance is omitted");
            else
                issue(Impact::blocker, "typhoon.empty-performance", item.id, "Performance has no playable entries");
            continue;
        }
        issue(Impact::loss, "typhoon.performance-controls", item.id,
              "Volume, entry key limits, transpose, priority, outputs, global settings and program-change mappings "
              "are not transferred");
        slots.insert(n);
        report.actions.push_back({"program", program.name, item.id});
        report.actions.back().details.push_back({"Program slot", std::to_string(n)});
        for (const auto &assignment : program.assignments)
            report.actions.back().details.push_back({"Assigned Sample Bank", assignment.target_name});
        bundle.programs.push_back(std::move(program));
    }
}
} // namespace axk::conversion::typhoon_to_a_series
