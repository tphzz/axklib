#include "alteration_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/tx16w.hpp"
#include "axklib/tx16w_a_series.hpp"
#include "axklib/writer.hpp"

namespace axk::alteration_internal {
namespace {

Result<tx16w::a_series::TargetInventory> target_inventory(TransactionState &state, MutablePartition &partition,
                                                          std::string_view volume_name,
                                                          const CancellationToken &cancellation) {
    tx16w::a_series::TargetInventory inventory;
    const auto collect_names = [&](std::string_view category, ObjectType type,
                                   std::vector<std::string> &destination) -> Result<void> {
        auto objects = category_objects(state, partition, volume_name, category, type, cancellation);
        if (!objects)
            return std::unexpected{objects.error()};
        destination.reserve(objects->size());
        for (const auto &object : *objects)
            destination.push_back(object.name);
        return {};
    };
    if (auto collected = collect_names("SMPL", ObjectType::smpl, inventory.wave_data_names); !collected)
        return std::unexpected{collected.error()};
    if (auto collected = collect_names("SBNK", ObjectType::sbnk, inventory.sample_names); !collected)
        return std::unexpected{collected.error()};
    if (auto collected = collect_names("SBAC", ObjectType::sbac, inventory.sample_bank_names); !collected)
        return std::unexpected{collected.error()};

    auto programs = category_objects(state, partition, volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    inventory.occupied_program_slots.reserve(programs->size());
    inventory.program_names.reserve(programs->size());
    for (const auto &program : *programs) {
        std::uint16_t number{};
        const auto parsed = std::from_chars(program.name.data(), program.name.data() + program.name.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != program.name.data() + program.name.size() || number == 0U ||
            number > 128U) {
            return std::unexpected{transaction_error("existing Program directory entry is not a valid slot")};
        }
        const auto *decoded = std::get_if<CurrentProg>(&program.decoded.payload);
        if (decoded == nullptr)
            return std::unexpected{transaction_error("existing Program payload is unreadable")};
        inventory.occupied_program_slots.push_back(static_cast<std::uint8_t>(number));
        inventory.program_names.push_back(decoded->program_name);
    }
    return inventory;
}

Result<ImportedAudio> imported_wave(const tx16w::Wave &source, std::uint32_t target_sample_rate,
                                    const std::filesystem::path &disk_path) {
    Waveform waveform;
    waveform.source_path = disk_path;
    waveform.name = source.name;
    waveform.format = {.channels = 1U, .sample_width_bytes = 2U, .sample_rate = source.sample_rate};
    waveform.frame_count = source.pcm.size();
    waveform.pcm.resize(source.pcm.size() * sizeof(std::int16_t));
    for (std::size_t index = 0U; index < source.pcm.size(); ++index) {
        const auto value = static_cast<std::uint16_t>(source.pcm[index]);
        waveform.pcm[index * 2U] = static_cast<std::byte>(value & 0xffU);
        waveform.pcm[index * 2U + 1U] = static_cast<std::byte>(value >> 8U);
    }
    auto bytes = wav_bytes(waveform);
    if (!bytes)
        return std::unexpected{bytes.error()};
    MemoryReader reader{std::move(*bytes)};
    AudioImportOptions options;
    options.expected_channels = 1U;
    options.target_sample_rate = target_sample_rate;
    auto imported = import_sampler_audio(reader, options);
    if (!imported)
        return std::unexpected{imported.error()};
    imported->source_path = disk_path;
    imported->source_format = "TX16W";
    imported->source_subtype = "signed 12-bit packed PCM";
    imported->source_sample_width_bits = 12U;
    imported->sample_width_converted = true;
    return imported;
}

void merge_report(OperationReport &destination, OperationReport source) {
    destination.inserted_sfs_ids.insert(destination.inserted_sfs_ids.end(), source.inserted_sfs_ids.begin(),
                                        source.inserted_sfs_ids.end());
    destination.allocated_clusters += source.allocated_clusters;
}

Result<tx16w::Inspection> inspect_disk_set(std::span<const std::filesystem::path> paths,
                                           const CancellationToken &cancellation) {
    std::vector<tx16w::SourceFile> files;
    for (const auto &path : paths) {
        auto disk = FatImage::open(path, cancellation);
        if (!disk)
            return std::unexpected{disk.error()};
        for (const auto &entry : disk->files()) {
            auto bytes = disk->read_file(entry, cancellation);
            if (!bytes)
                return std::unexpected{bytes.error()};
            files.push_back({entry.path, std::move(*bytes), path.string()});
        }
    }
    return tx16w::inspect_files(files, cancellation);
}

} // namespace

Result<OperationReport> import_tx16w_disk_set(TransactionState &state, OperationContext context,
                                              const ImportTx16wDiskSetOperation &operation,
                                              const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("TX16W import target partition is invalid")};

    auto inventory = target_inventory(state, found->second, operation.volume_name, cancellation);
    if (!inventory)
        return std::unexpected{inventory.error()};
    auto inspection = inspect_disk_set(operation.disk_paths, cancellation);
    if (!inspection)
        return std::unexpected{inspection.error()};
    auto plan = tx16w::a_series::plan_import(*inspection, *inventory, operation.import_mode);
    if (!plan)
        return std::unexpected{plan.error()};
    const auto blocked = std::ranges::find(plan->notices, tx16w::a_series::MappingDisposition::blocked,
                                           &tx16w::a_series::MappingNotice::disposition);
    if (blocked != plan->notices.end()) {
        return std::unexpected{transaction_error(std::format("TX16W import is blocked: {}", blocked->message))};
    }

    OperationReport result;
    result.id = context.id;
    result.type = context.type;
    result.partition = *partition_index;
    result.volume_name = operation.volume_name;
    result.object_name = operation.disk_paths.size() == 1U ? operation.disk_paths.front().filename().string()
                                                           : std::format("{} TX16W disks", operation.disk_paths.size());

    for (const auto &wave : plan->wave_data) {
        if (const auto cancelled = cancellation.check(); !cancelled)
            return std::unexpected{cancelled.error()};
        if (wave.source_wave_index >= inspection->waves.size())
            return std::unexpected{transaction_error("TX16W plan references a missing source Wave")};
        const auto &source_wave = inspection->waves[wave.source_wave_index];
        const auto source_path = source_wave.source_member.empty() ? operation.disk_paths.front()
                                                                   : std::filesystem::path{source_wave.source_member};
        auto audio = imported_wave(source_wave, wave.target_sample_rate, source_path);
        if (!audio)
            return std::unexpected{audio.error()};
        InsertWaveformSpec spec;
        spec.path = source_path;
        spec.waveform_names = {wave.name};
        spec.root_key = wave.root_key;
        spec.target_sample_rate = wave.target_sample_rate;
        spec.loop_mode = wave.loop_mode;
        if (wave.loop_mode == AudioSamplerLoopMode::forward_loop) {
            if (audio->output_frames == 0U)
                return std::unexpected{transaction_error("TX16W looped Wave has no decoded PCM frames")};
            if (audio->output_frames > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected{transaction_error("TX16W Wave exceeds the A-series loop frame range")};
            const auto start = std::min<std::uint64_t>(wave.loop_start_frame, audio->output_frames - 1U);
            spec.loop_start_frame = static_cast<std::uint32_t>(start);
            spec.loop_length_frames = static_cast<std::uint32_t>(audio->output_frames - start);
        } else {
            spec.loop_start_frame = 0U;
            spec.loop_length_frames = 0U;
        }
        auto inserted = insert_waveform_audio(
            state, context, {operation.partition, operation.volume_name, std::move(spec)}, *audio, cancellation);
        if (!inserted)
            return std::unexpected{inserted.error()};
        merge_report(result, std::move(*inserted));
    }
    for (const auto &sample : plan->samples) {
        auto inserted = insert_sbnk(state, context, {operation.partition, operation.volume_name, sample}, cancellation);
        if (!inserted)
            return std::unexpected{inserted.error()};
        merge_report(result, std::move(*inserted));
    }
    for (const auto &sample_bank : plan->sample_banks) {
        auto inserted =
            insert_sbac(state, context, {operation.partition, operation.volume_name, sample_bank}, cancellation);
        if (!inserted)
            return std::unexpected{inserted.error()};
        merge_report(result, std::move(*inserted));
    }
    for (const auto &program : plan->programs) {
        auto inserted =
            insert_program(state, context, {operation.partition, operation.volume_name, program}, cancellation);
        if (!inserted)
            return std::unexpected{inserted.error()};
        merge_report(result, std::move(*inserted));
    }
    return result;
}

} // namespace axk::alteration_internal
