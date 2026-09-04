#include "axklib/tx16w_a_series.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axk::tx16w::a_series {
namespace {

Error mapping_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}

std::string uppercase(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

std::string wave_stem(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    auto base = path.substr(slash == std::string_view::npos ? 0U : slash + 1U);
    const auto dot = base.find_last_of('.');
    if (dot != std::string_view::npos)
        base = base.substr(0U, dot);
    auto result = uppercase(std::string{base});
    std::ranges::replace(result, '_', ' ');
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

std::string wave_symbol(std::string value) {
    value = uppercase(std::move(value));
    std::ranges::replace(value, '_', ' ');
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    return value;
}

bool is_compressed_wave_file(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto base = path.substr(slash == std::string_view::npos ? 0U : slash + 1U);
    const auto dot = base.find_last_of('.');
    return dot != std::string_view::npos && dot + 1U < base.size() &&
           static_cast<char>(std::toupper(static_cast<unsigned char>(base[dot + 1U]))) == 'C';
}

std::string unique_name(std::string_view source, std::size_t maximum, std::set<std::string, std::less<>> &used) {
    std::string base{source.substr(0U, std::min(maximum, source.size()))};
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    if (base.empty())
        base = "TX16W";
    if (used.emplace(uppercase(base)).second)
        return base;
    for (std::size_t suffix = 2U;; ++suffix) {
        const auto suffix_text = " " + std::to_string(suffix);
        const auto prefix_size = maximum > suffix_text.size() ? maximum - suffix_text.size() : 0U;
        auto candidate = base.substr(0U, std::min(prefix_size, base.size())) + suffix_text;
        if (used.emplace(uppercase(candidate)).second)
            return candidate;
    }
}

std::uint32_t target_rate(std::uint32_t source) {
    return *std::ranges::min_element(supported_sampler_sample_rates, {}, [source](std::uint32_t candidate) {
        return candidate > source ? candidate - source : source - candidate;
    });
}

std::uint32_t scale_frames(std::uint64_t frames, std::uint32_t source_rate, std::uint32_t destination_rate) {
    const auto scaled = (static_cast<std::uint64_t>(frames) * destination_rate + source_rate / 2U) / source_rate;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(scaled, std::numeric_limits<std::uint32_t>::max()));
}

struct LoopTranslation {
    std::uint32_t start_frame{};
    std::uint32_t length_frames{};
    AudioSamplerLoopMode mode{AudioSamplerLoopMode::forward_one_shot};
};

LoopTranslation translate_loop(const Wave &wave, std::uint32_t destination_rate) {
    if (!wave.looped || wave.repeat_frames == 0U)
        return {};

    const auto total_source_frames = static_cast<std::uint64_t>(wave.attack_frames) + wave.repeat_frames;
    const auto total_frames = scale_frames(total_source_frames, wave.sample_rate, destination_rate);
    if (total_frames == 0U)
        return {};

    const auto scaled_attack = scale_frames(wave.attack_frames, wave.sample_rate, destination_rate);
    const auto start_frame = std::min(scaled_attack, total_frames - 1U);
    return {start_frame, total_frames - start_frame, AudioSamplerLoopMode::forward_loop};
}

std::optional<std::uint8_t> midi_key(std::uint8_t native_key) {
    if (native_key < 16U || native_key > 143U)
        return std::nullopt;
    return static_cast<std::uint8_t>(native_key - 16U);
}

void notice(ImportPlan &plan, MappingDisposition disposition, std::string source_object, std::string source_parameter,
            std::string target_object, std::string target_parameter, std::string message) {
    plan.notices.push_back({disposition, std::move(source_object), std::move(source_parameter),
                            std::move(target_object), std::move(target_parameter), std::move(message)});
}

struct SetupMaps {
    std::map<std::uint8_t, const Timbre *> timbres;
    std::map<std::uint8_t, std::string> banks;
    std::map<std::uint8_t, std::size_t> waves;
    std::map<std::uint8_t, std::string> wave_names;
};

std::optional<std::uint8_t> next_program_slot(const std::set<std::uint8_t> &occupied) {
    for (std::uint16_t slot = 1U; slot <= 128U; ++slot) {
        if (!occupied.contains(static_cast<std::uint8_t>(slot)))
            return static_cast<std::uint8_t>(slot);
    }
    return std::nullopt;
}

} // namespace

Result<ImportPlan> plan_import(const Inspection &inspection, const TargetInventory &target, ImportMode mode) {
    ImportPlan plan;
    for (const auto &source_notice : inspection.notices) {
        const auto disposition = source_notice.disposition == ParseNoticeDisposition::defaulted
                                     ? MappingDisposition::defaulted
                                     : MappingDisposition::omitted;
        notice(plan, disposition, source_notice.source_object, source_notice.source_parameter, {}, {},
               source_notice.message);
    }
    std::map<std::string, std::vector<std::size_t>, std::less<>> source_waves;
    for (std::size_t index = 0U; index < inspection.waves.size(); ++index)
        source_waves[wave_stem(inspection.waves[index].name)].push_back(index);

    std::set<std::string, std::less<>> wave_names;
    std::set<std::string, std::less<>> sample_names;
    std::set<std::string, std::less<>> bank_names;
    std::set<std::string, std::less<>> program_names;
    for (const auto &name : target.wave_data_names)
        wave_names.insert(uppercase(name));
    for (const auto &name : target.sample_names)
        sample_names.insert(uppercase(name));
    for (const auto &name : target.sample_bank_names)
        bank_names.insert(uppercase(name));
    for (const auto &name : target.program_names)
        program_names.insert(uppercase(name));
    std::set<std::uint8_t> occupied(target.occupied_program_slots.begin(), target.occupied_program_slots.end());
    if (std::ranges::any_of(occupied, [](std::uint8_t slot) { return slot == 0U || slot > 128U; }))
        return std::unexpected{mapping_error("occupied A-series Program slots must be between 1 and 128")};

    if (mode == ImportMode::wave_data_only) {
        for (std::size_t index = 0U; index < inspection.waves.size(); ++index) {
            const auto &wave = inspection.waves[index];
            const auto rate = target_rate(wave.sample_rate);
            const auto loop = translate_loop(wave, rate);
            plan.wave_data.push_back({unique_name(wave_stem(wave.name), 16U, wave_names), index, rate, 60U,
                                      loop.start_frame, loop.length_frames, loop.mode});
            notice(plan, MappingDisposition::defaulted, wave.name, "root_key", plan.wave_data.back().name, "root_key",
                   "Wave Data-only import defaults the A-series root key to C3 (60)");
        }
        return plan;
    }

    std::set<std::string, std::less<>> compressed_wave_symbols;
    for (const auto &unsupported : inspection.unsupported_files) {
        if (is_compressed_wave_file(unsupported)) {
            compressed_wave_symbols.insert(wave_stem(unsupported));
            notice(plan, MappingDisposition::omitted, unsupported, "compressed_wave_payload", {}, {},
                   "Compressed TX16W Wave data is present but is not decoded by the current import profile");
        } else {
            notice(plan, MappingDisposition::omitted, unsupported, "auxiliary_file", {}, {},
                   "TX16W auxiliary data is outside the current import profile");
        }
    }

    for (const auto &setup : inspection.setups) {
        SetupMaps maps;
        for (const auto &timbre : setup.timbres)
            maps.timbres.emplace(timbre.slot, &timbre);
        for (const auto &reference : setup.waves) {
            const auto found = source_waves.find(wave_symbol(reference.name));
            if (found == source_waves.end()) {
                const auto compressed = compressed_wave_symbols.contains(wave_symbol(reference.name));
                notice(plan, MappingDisposition::blocked, setup.name, "wave_slot", {}, "Wave Data",
                       compressed ? "Setup references TX16W Wave " + reference.name +
                                        " which is present only as unsupported compressed .Cnn data"
                                  : "Setup references TX16W Wave " + reference.name +
                                        " which is not present in the selected disk set");
                continue;
            }
            const auto source_index = std::ranges::find_if(found->second, [&](std::size_t index) {
                return inspection.waves[index].native_slot == reference.slot;
            });
            const auto source_slot_matches = std::ranges::count_if(found->second, [&](std::size_t index) {
                return inspection.waves[index].native_slot == reference.slot;
            });
            const auto all_slots_unknown = std::ranges::all_of(
                found->second, [&](std::size_t index) { return !inspection.waves[index].native_slot.has_value(); });
            if (source_slot_matches > 1 ||
                (source_slot_matches == 0 && !(all_slots_unknown && found->second.size() == 1U))) {
                notice(plan, MappingDisposition::blocked, setup.name, "wave_slot", {}, "Wave Data",
                       "Setup references TX16W Wave " + reference.name +
                           " which is ambiguous across the selected disk set");
                continue;
            }
            maps.waves.emplace(reference.slot, source_slot_matches == 1 ? *source_index : found->second.front());
            maps.wave_names.emplace(reference.slot, reference.name);
        }

        for (const auto &voice : setup.voices) {
            SampleBankSpec bank;
            bank.name = unique_name(voice.name, 16U, bank_names);
            for (const auto &region : voice.regions) {
                const auto timbre = maps.timbres.find(region.timbre_slot);
                if (timbre == maps.timbres.end()) {
                    notice(plan, MappingDisposition::omitted, voice.name, "timbre_slot", bank.name, "Sample",
                           "Voice region references a missing TX16W Timbre slot");
                    continue;
                }
                const auto source_wave = maps.waves.find(timbre->second->wave_slot);
                if (source_wave == maps.waves.end()) {
                    notice(plan, MappingDisposition::omitted, timbre->second->name, "wave_slot", bank.name, "Wave Data",
                           "Timbre references an unavailable TX16W Wave slot");
                    continue;
                }
                const auto root = midi_key(timbre->second->root_key_number);
                const auto low = midi_key(region.low_key_number);
                const auto high = midi_key(region.high_key_number);
                if (!root || !low || !high) {
                    notice(plan, MappingDisposition::omitted, timbre->second->name, "key_number", bank.name,
                           "key range", "TX16W key number is outside the proven native 16..143 range");
                    continue;
                }
                const auto &wave = inspection.waves[source_wave->second];
                const auto wave_name = maps.wave_names.find(timbre->second->wave_slot);
                if (wave_name == maps.wave_names.end()) {
                    notice(plan, MappingDisposition::omitted, timbre->second->name, "wave_slot", bank.name, "Wave Data",
                           "Timbre references an unnamed TX16W Wave slot");
                    continue;
                }
                const auto wave_key = uppercase(wave_name->second);
                auto wave_plan =
                    std::ranges::find(plan.wave_data, source_wave->second, &WaveDataPlan::source_wave_index);
                if (wave_plan == plan.wave_data.end()) {
                    const auto name = unique_name(wave_key, 16U, wave_names);
                    const auto rate = target_rate(wave.sample_rate);
                    const auto loop = translate_loop(wave, rate);
                    plan.wave_data.push_back(
                        {name, source_wave->second, rate, *root, loop.start_frame, loop.length_frames, loop.mode});
                    wave_plan = std::prev(plan.wave_data.end());
                    if (rate != wave.sample_rate) {
                        notice(plan, MappingDisposition::approximated, wave.name, "sample_rate", name, "sample_rate",
                               "TX16W sample rate is resampled to the nearest A-series rate");
                    }
                    if (wave.looped && wave.repeat_frames == 0U) {
                        notice(plan, MappingDisposition::defaulted, wave.name, "repeat_frames", name, "loop mode",
                               "TX16W loop flag has no repeat region; A-series playback defaults to one-shot");
                    }
                } else if (wave_plan->root_key != *root) {
                    notice(plan, MappingDisposition::approximated, wave.name, "root_key", wave_plan->name, "root_key",
                           "Shared Wave Data retains its first translated root key; each Sample "
                           "retains its own translated root key");
                }
                SampleSpec sample;
                sample.name = unique_name(timbre->second->name, 16U, sample_names);
                sample.waveform_id = wave_plan->name;
                sample.target_sample_rate = wave_plan->target_sample_rate;
                sample.parameters.root_key = *root;
                sample.parameters.key_low = *low;
                sample.parameters.key_high = *high;
                sample.parameters.loop_mode = wave_plan->loop_mode;
                sample.parameters.loop_start_frame = wave_plan->loop_start_frame;
                sample.parameters.loop_length_frames = wave_plan->loop_length_frames;
                bank.member_samples.push_back(sample.name);
                plan.samples.push_back(std::move(sample));
                notice(plan, MappingDisposition::approximated, timbre->second->name, "key_number",
                       plan.samples.back().name, "MIDI key", "TX16W native key numbers are translated by -16");
                if (region.fade != 0U) {
                    notice(plan, MappingDisposition::omitted, voice.name, "fade", plan.samples.back().name,
                           "key crossfade", "TX16W Voice Fade is preserved in the preview but is not authored");
                }
                if (region.native_timbre_selector_flags != 0U) {
                    notice(plan, MappingDisposition::omitted, voice.name, "timbre_selector_flags",
                           plan.samples.back().name, "Sample assignment",
                           "TX16W native Timbre selector flags are preserved in the source model but are not authored");
                }
            }
            if (!bank.member_samples.empty()) {
                maps.banks.emplace(voice.slot, bank.name);
                plan.sample_banks.push_back(std::move(bank));
            }
        }

        for (const auto &performance : setup.performances) {
            const auto number = next_program_slot(occupied);
            if (!number) {
                notice(plan, MappingDisposition::blocked, performance.name, "performance", {}, "Program slot",
                       "No free A-series Program slot remains");
                continue;
            }
            ProgramSpec program;
            program.number = *number;
            program.name = unique_name(performance.name, 8U, program_names);
            std::set<std::uint8_t> assigned_voices;
            for (const auto &source_assignment : performance.voices) {
                if (!assigned_voices.emplace(source_assignment.voice_slot).second)
                    continue;
                const auto bank = maps.banks.find(source_assignment.voice_slot);
                if (bank == maps.banks.end())
                    continue;
                ProgramAssignmentSpec assignment;
                assignment.target_kind = "SBAC";
                assignment.target_name = bank->second;
                if (source_assignment.receive_channel == 16U) {
                    assignment.receive_mode = ProgramReceiveMode::sample;
                } else if (source_assignment.receive_channel < 16U) {
                    assignment.receive_mode = ProgramReceiveMode::midi_channel;
                    assignment.receive_channel = static_cast<std::uint8_t>(source_assignment.receive_channel + 1U);
                } else {
                    notice(plan, MappingDisposition::omitted, performance.name, "receive_channel", program.name,
                           "receive channel", "TX16W receive channel is outside its native 0..16 range");
                    continue;
                }
                program.assignments.push_back(std::move(assignment));
            }
            if (program.assignments.size() > maximum_program_assignments) {
                notice(plan, MappingDisposition::blocked, performance.name, "voice_assignments", program.name,
                       "Program assignments", "TX16W Performance exceeds its native 16-Voice assignment capacity");
                continue;
            }
            if (!program.assignments.empty()) {
                occupied.insert(*number);
                notice(plan, MappingDisposition::omitted, performance.name, "performance_voice_controls", program.name,
                       "Program assignment controls",
                       "TX16W per-Voice volume, detune, transpose, output, and alternative-group controls are "
                       "reported but are not authored by the current A-series adapter");
                plan.programs.push_back(std::move(program));
            }
        }
        if (setup.performances.empty()) {
            for (const auto &voice : setup.voices) {
                const auto bank = maps.banks.find(voice.slot);
                if (bank == maps.banks.end())
                    continue;
                const auto number = next_program_slot(occupied);
                if (!number) {
                    notice(plan, MappingDisposition::blocked, voice.name, "missing_performance", {}, "Program slot",
                           "No free A-series Program slot remains");
                    continue;
                }
                ProgramSpec program;
                program.number = *number;
                occupied.insert(*number);
                program.name = unique_name(voice.name, 8U, program_names);
                program.assignments.push_back({"SBAC", bank->second, 0U, ProgramReceiveMode::sample});
                notice(plan, MappingDisposition::defaulted, voice.name, "missing_performance", program.name, "Program",
                       "No TX16W Performance was present; an audition Program was created from the Voice");
                plan.programs.push_back(std::move(program));
            }
        }
    }
    return plan;
}

} // namespace axk::tx16w::a_series
