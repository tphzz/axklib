#include "axklib/conversion/tx16w_to_a_series/profile.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "axklib/audio.hpp"
#include "axklib/conversion/tx16w_to_a_series/plan.hpp"
#include "axklib/formats/a_series/authoring/prepare.hpp"

namespace axk::tx16w::a_series {
namespace {
namespace authoring = axk::a_series::authoring;
using namespace conversion;
constexpr std::string_view profile_id = "yamaha.tx16w-to-a-series";

Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}

Impact impact(const MappingNotice &notice) {
    if (notice.disposition == MappingDisposition::blocked)
        return Impact::blocker;
    if (notice.disposition == MappingDisposition::exact)
        return Impact::exact;
    if (notice.disposition == MappingDisposition::approximated && notice.source_parameter == "key_number")
        return Impact::representation;
    if (notice.disposition == MappingDisposition::defaulted)
        return Impact::uncertainty;
    return Impact::loss;
}

std::uint64_t report_bytes(const Report &report) {
    std::uint64_t bytes = sizeof(PreparedPlan) + report.profile_id.capacity() +
                          report.actions.capacity() * sizeof(Action) + report.issues.capacity() * sizeof(Issue);
    for (const auto &action : report.actions) {
        bytes += action.kind.capacity() + action.name.capacity() + action.source_object.capacity() +
                 action.action.capacity() + action.details.capacity() * sizeof(Action::Detail);
        for (const auto &detail : action.details)
            bytes += detail.label.capacity() + detail.value.capacity();
    }
    for (const auto &notice : report.issues)
        bytes += notice.id.capacity() + notice.code.capacity() + notice.source_object.capacity() +
                 notice.source_parameter.capacity() + notice.target_object.capacity() +
                 notice.target_parameter.capacity() + notice.message.capacity();
    bytes += report.capacity.reports.capacity() * sizeof(VolumeCapacityReport);
    for (const auto &capacity : report.capacity.reports) {
        bytes += capacity.volume_name.capacity() +
                 capacity.object_counts.capacity() * sizeof(VolumeCapacityObjectCount) +
                 capacity.profiles.capacity() * sizeof(VolumeCapacityProfile);
        for (const auto &object : capacity.object_counts)
            bytes += object.type.capacity();
        for (const auto &profile : capacity.profiles) {
            bytes += profile.reasons.capacity() * sizeof(VolumeCapacityReason);
            for (const auto &reason : profile.reasons)
                bytes += reason.code.capacity() + reason.message.capacity();
        }
    }
    return bytes;
}

Result<ImportedAudio> materialize(const Wave &source, std::uint32_t rate) {
    Waveform waveform;
    waveform.name = source.name;
    waveform.format = {.channels = 1U, .sample_width_bytes = 2U, .sample_rate = source.sample_rate};
    waveform.frame_count = source.pcm.size();
    waveform.pcm.resize(source.pcm.size() * sizeof(std::int16_t));
    for (std::size_t index = 0U; index < source.pcm.size(); ++index) {
        const auto sample = static_cast<std::uint16_t>(source.pcm[index]);
        waveform.pcm[index * 2U] = static_cast<std::byte>(sample & 0xffU);
        waveform.pcm[index * 2U + 1U] = static_cast<std::byte>(sample >> 8U);
    }
    auto bytes = wav_bytes(waveform);
    if (!bytes)
        return std::unexpected(bytes.error());
    MemoryReader reader{std::move(*bytes)};
    auto audio = import_sampler_audio(reader, {.expected_channels = 1U, .target_sample_rate = rate});
    if (!audio)
        return std::unexpected(audio.error());
    audio->source_format = "TX16W";
    audio->source_subtype = "signed 12-bit packed PCM";
    audio->source_sample_width_bits = 12U;
    audio->sample_width_converted = true;
    return audio;
}

Result<std::shared_ptr<const PreparedPlan>> prepare(const Request &request, const CancellationToken &cancellation,
                                                    ProgressSink *progress) {
    const auto document = std::dynamic_pointer_cast<const Document>(request.source);
    if (!document)
        return std::unexpected(invalid("TX16W profile requires a native TX16W document"));
    ImportMode mode;
    authoring::Destination destination;
    try {
        if (!request.options.is_object() || request.options.size() != 1U ||
            !request.destination.selection.is_object() || request.destination.selection.size() != 2U)
            return std::unexpected(invalid("Conversion options and destination contain unknown or missing fields"));
        const auto name = request.options.at("mode").get<std::string>();
        if (name != "HIERARCHY" && name != "WAVE_DATA_ONLY")
            return std::unexpected(invalid("Conversion mode must be HIERARCHY or WAVE_DATA_ONLY"));
        mode = name == "HIERARCHY" ? ImportMode::hierarchy : ImportMode::wave_data_only;
        const auto &partition = request.destination.selection.at("partitionIndex");
        if (!partition.is_number_integer() || partition < 0 || partition > 7)
            return std::unexpected(invalid("Destination partition index must be between 0 and 7"));
        destination = {partition.get<std::uint8_t>(),
                       request.destination.selection.at("volumeName").get<std::string>()};
        if (destination.volume_name.empty())
            return std::unexpected(invalid("Conversion requires an existing destination volume"));
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(invalid("Malformed conversion options or destination"));
    }
    auto source = inspect_document(*document, mode, cancellation);
    if (!source)
        return std::unexpected(source.error());
    const auto inventory = authoring::inventory(request.destination.reader, destination, cancellation);
    if (!inventory)
        return std::unexpected(inventory.error());
    const TargetInventory target{inventory->occupied_program_slots, inventory->wave_data_names, inventory->sample_names,
                                 inventory->sample_bank_names, inventory->program_names};
    auto mapped = plan_import(*source, target, mode);
    if (!mapped)
        return std::unexpected(mapped.error());
    Report report;
    report.profile_id = profile_id;
    report.fidelity = request.fidelity;
    report.executable = true;
    auto issue = [&](Impact severity, std::string code, std::string source_id, std::string message,
                     std::string source_parameter = {}, std::string target_object = {},
                     std::string target_parameter = {}) {
        report.issues.push_back({"issue-" + std::to_string(report.issues.size() + 1U), std::move(code), severity,
                                 std::move(source_id), std::move(source_parameter), std::move(target_object),
                                 std::move(target_parameter), std::move(message)});
        if (severity == Impact::blocker ||
            (request.fidelity == Fidelity::strict && (severity == Impact::loss || severity == Impact::uncertainty)))
            report.executable = false;
    };
    for (const auto &notice : mapped->notices)
        issue(impact(notice), "tx16w." + notice.source_parameter, notice.source_object, notice.message,
              notice.source_parameter, notice.target_object, notice.target_parameter);
    for (const auto &wave : mapped->wave_data) {
        const auto &id = source->wave_object_ids.at(wave.source_wave_index);
        report.actions.push_back({"wave-data", wave.name, id});
        report.actions.back().details = {{"Sample rate", std::to_string(wave.target_sample_rate)},
                                         {"Root key", std::to_string(wave.root_key)},
                                         {"Loop start", std::to_string(wave.loop_start_frame)},
                                         {"Loop frames", std::to_string(wave.loop_length_frames)}};
        issue(Impact::representation, "tx16w.pcm12-expansion", id,
              "Signed 12-bit PCM is expanded exactly to 16-bit PCM before any sample-rate conversion");
        if (mode == ImportMode::wave_data_only &&
            wave.target_sample_rate != source->waves[wave.source_wave_index].sample_rate)
            issue(Impact::loss, "tx16w.sample-rate", id, "Wave Data is resampled to the nearest A-series rate");
    }
    for (const auto &sample : mapped->samples) {
        report.actions.push_back({"sample", sample.name, mapped->source_objects.at("sample:" + sample.name)});
        report.actions.back().details = {{"Wave Data", sample.waveform_id.value_or("")},
                                         {"Root key", std::to_string(sample.parameters.root_key.value())},
                                         {"Key range", std::to_string(sample.parameters.key_low.value()) + ".." +
                                                           std::to_string(sample.parameters.key_high.value())}};
        issue(Impact::uncertainty, "a-series.sample-defaults", report.actions.back().source_object,
              "Destination-only Sample controls use the A-series writer defaults", {}, sample.name);
    }
    for (const auto &bank : mapped->sample_banks) {
        report.actions.push_back({"sample-bank", bank.name, mapped->source_objects.at("sample-bank:" + bank.name)});
        for (const auto &sample : bank.member_samples)
            report.actions.back().details.push_back({"Sample", sample});
    }
    for (const auto &program : mapped->programs) {
        report.actions.push_back({"program", program.name, mapped->source_objects.at("program:" + program.name)});
        report.actions.back().details.push_back({"Program slot", std::to_string(program.number)});
        for (const auto &assignment : program.assignments)
            report.actions.back().details.push_back({assignment.target_kind, assignment.target_name});
    }
    if (report.actions.empty())
        issue(Impact::blocker, "conversion.empty", {}, "No convertible objects were found for this mode");
    if (report.actions.size() > request.limits.maximum_actions || report.issues.size() > request.limits.maximum_issues)
        return std::unexpected(invalid("Conversion report exceeds its bounded size"));
    std::uint64_t retained_bytes = report_bytes(report);
    if (retained_bytes > request.limits.maximum_bytes)
        return std::unexpected(invalid("Conversion report exceeds its memory limit"));
    if (!report.executable)
        return std::make_shared<const PreparedPlan>(std::move(report), std::vector<Patch>{},
                                                    request.destination.reader->size(), retained_bytes);

    authoring::Bundle bundle;
    bundle.samples = std::move(mapped->samples);
    bundle.sample_banks = std::move(mapped->sample_banks);
    bundle.programs = std::move(mapped->programs);
    std::uint64_t pcm_bytes{};
    for (const auto &wave : mapped->wave_data) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        const auto &header = source->waves.at(wave.source_wave_index);
        const auto frames = static_cast<std::uint64_t>(header.attack_frames) + header.repeat_frames;
        const auto output_bound = (frames * wave.target_sample_rate + header.sample_rate - 1U) / header.sample_rate;
        const auto bytes = (frames + output_bound) * sizeof(std::int16_t);
        // Reserve room for conversion scratch, encoded objects and both patch images before allocating PCM.
        if (bytes > (request.limits.maximum_bytes - retained_bytes) / 8U - pcm_bytes)
            return std::unexpected(invalid("Conversion audio exceeds its preparation memory limit"));
        pcm_bytes += bytes;
        auto decoded = document->read_wave(source->wave_object_ids.at(wave.source_wave_index), cancellation);
        if (!decoded)
            return std::unexpected(decoded.error());
        auto audio = materialize(*decoded, wave.target_sample_rate);
        if (!audio)
            return std::unexpected(audio.error());
        WaveformSpec spec;
        spec.name = wave.name;
        spec.root_key = wave.root_key;
        spec.target_sample_rate = wave.target_sample_rate;
        spec.loop_mode = wave.loop_mode;
        if (wave.loop_mode == AudioSamplerLoopMode::forward_loop) {
            if (audio->output_frames == 0U || audio->output_frames > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected(invalid("Converted loop has no representable PCM frames"));
            spec.loop_start_frame =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(wave.loop_start_frame, audio->output_frames - 1U));
            spec.loop_length_frames = static_cast<std::uint32_t>(audio->output_frames - spec.loop_start_frame);
        }
        for (auto &sample : bundle.samples) {
            if (sample.waveform_id == wave.name) {
                sample.parameters.loop_start_frame = spec.loop_start_frame;
                sample.parameters.loop_length_frames = spec.loop_length_frames;
            }
        }
        const auto action = std::ranges::find_if(
            report.actions, [&](const auto &item) { return item.kind == "wave-data" && item.name == wave.name; });
        if (action != report.actions.end()) {
            action->details[2U].value = std::to_string(spec.loop_start_frame);
            action->details[3U].value = std::to_string(spec.loop_length_frames);
        }
        bundle.wave_data.push_back({std::move(spec), std::move(*audio)});
    }
    auto authored = authoring::prepare(request.destination.reader, request.destination.label, destination, bundle,
                                       request.capacity_policy, cancellation, progress);
    if (!authored)
        return std::unexpected(authored.error());
    report.capacity = std::move(authored->capacity);
    if (const auto admitted = enforce_volume_capacity_admission(report.capacity, request.capacity_policy); !admitted)
        issue(Impact::blocker, "conversion.capacity", {}, admitted.error().message);
    retained_bytes = report_bytes(report);
    if (retained_bytes > request.limits.maximum_bytes || report.issues.size() > request.limits.maximum_issues)
        return std::unexpected(invalid("Conversion capacity report exceeds its retention limit"));
    if (!report.executable)
        return std::make_shared<const PreparedPlan>(std::move(report), std::vector<Patch>{}, authored->image_size_bytes,
                                                    retained_bytes);
    std::vector<Patch> patches;
    patches.reserve(authored->patches.size());
    const auto patch_overhead = patches.capacity() * sizeof(Patch);
    if (patch_overhead > request.limits.maximum_bytes - retained_bytes)
        return std::unexpected(invalid("Conversion patch list exceeds its retention limit"));
    retained_bytes += patch_overhead;
    for (auto &patch : authored->patches) {
        const auto bytes = patch.original.capacity() + patch.replacement.capacity();
        if (bytes > request.limits.maximum_bytes - retained_bytes)
            return std::unexpected(invalid("Conversion patches exceed their retained memory limit"));
        retained_bytes += bytes;
        patches.push_back({patch.offset, std::move(patch.original), std::move(patch.replacement)});
    }
    return std::make_shared<const PreparedPlan>(std::move(report), std::move(patches), authored->image_size_bytes,
                                                retained_bytes);
}
} // namespace

conversion::Profile profile() {
    return {std::string{profile_id},
            "TX16W to A-series",
            "yamaha.tx16w",
            "yamaha.a-series",
            {{"type", "object"},
             {"additionalProperties", false},
             {"required", {"mode"}},
             {"properties", {{"mode", {{"type", "string"}, {"enum", {"HIERARCHY", "WAVE_DATA_ONLY"}}}}}}},
            prepare};
}

} // namespace axk::tx16w::a_series
