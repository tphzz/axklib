#include "axklib/formats/a_series/authoring/prepare.hpp"

#include <charconv>
#include <cstddef>
#include <utility>

#include "../../../alteration_internal.hpp"

namespace axk::a_series::authoring {
namespace {
using namespace alteration_internal;

Result<MutablePartition *> destination_partition(TransactionState &state, const Destination &destination,
                                                 const CancellationToken &cancellation) {
    const auto found = state.partitions.find(destination.partition_index);
    if (found == state.partitions.end())
        return std::unexpected(transaction_error("Conversion target partition does not exist"));
    const auto volume = unique_directory_child(state, found->second, SfsId{1U}, destination.volume_name, cancellation);
    if (!volume)
        return std::unexpected(volume.error());
    state.capacity_affected_volumes.emplace(PartitionIndex{destination.partition_index}, *volume);
    return &found->second;
}
} // namespace

Result<Inventory> inventory(std::shared_ptr<const RandomAccessReader> reader, const Destination &destination,
                            const CancellationToken &cancellation) {
    auto state = open_transaction_state(std::move(reader), {}, cancellation, nullptr, true);
    if (!state)
        return std::unexpected(state.error());
    auto partition = destination_partition(*state, destination, cancellation);
    if (!partition)
        return std::unexpected(partition.error());
    Inventory result;
    const auto collect = [&](std::string_view category, ObjectType type,
                             std::vector<std::string> &names) -> Result<void> {
        const auto objects =
            category_objects(*state, **partition, destination.volume_name, category, type, cancellation);
        if (!objects)
            return std::unexpected(objects.error());
        for (const auto &object : *objects)
            names.push_back(object.name);
        return {};
    };
    if (const auto read = collect("SMPL", ObjectType::smpl, result.wave_data_names); !read)
        return std::unexpected(read.error());
    if (const auto read = collect("SBNK", ObjectType::sbnk, result.sample_names); !read)
        return std::unexpected(read.error());
    if (const auto read = collect("SBAC", ObjectType::sbac, result.sample_bank_names); !read)
        return std::unexpected(read.error());
    const auto programs =
        category_objects(*state, **partition, destination.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected(programs.error());
    for (const auto &program : *programs) {
        std::uint16_t slot{};
        const auto parsed = std::from_chars(program.name.data(), program.name.data() + program.name.size(), slot);
        const auto *decoded = std::get_if<CurrentProg>(&program.decoded.payload);
        if (parsed.ec != std::errc{} || parsed.ptr != program.name.data() + program.name.size() || slot == 0U ||
            slot > 128U || !decoded)
            return std::unexpected(transaction_error("Destination contains an unreadable Program or invalid slot"));
        result.occupied_program_slots.push_back(static_cast<std::uint8_t>(slot));
        result.program_names.push_back(decoded->program_name);
    }
    return result;
}

Result<detail::PreparedAlteration> prepare(std::shared_ptr<const RandomAccessReader> reader, std::filesystem::path path,
                                           const Destination &destination, const Bundle &bundle,
                                           const VolumeCapacityPolicy &policy, const CancellationToken &cancellation,
                                           ProgressSink *progress) {
    auto state = open_transaction_state(std::move(reader), path, cancellation, progress, true);
    if (!state)
        return std::unexpected(state.error());
    if (auto selected = destination_partition(*state, destination, cancellation); !selected)
        return std::unexpected(selected.error());
    const PartitionSelector partition = PartitionIndex{destination.partition_index};
    const OperationContext context{"native-import", "insert_native_objects"};
    const auto remember = [&](Result<OperationReport> inserted) -> Result<void> {
        if (!inserted)
            return std::unexpected(inserted.error());
        if (const auto recorded = remember_object_targets(*state, *inserted, cancellation); !recorded)
            return recorded;
        state->reports.push_back(std::move(*inserted));
        return {};
    };
    for (const auto &wave : bundle.wave_data) {
        if (wave.audio.pcm_channels.size() != 1U || wave.audio.output_sample_width_bits != 16U ||
            wave.audio.output_frames > maximum_wave_data_frames_per_channel ||
            wave.audio.pcm_channels.front().size() != wave.audio.output_frames * sizeof(std::int16_t))
            return std::unexpected(transaction_error("Prepared Wave Data must contain bounded mono PCM16"));
        InsertWaveformSpec spec;
        spec.path = wave.spec.path;
        spec.waveform_names = {wave.spec.name};
        spec.root_key = wave.spec.root_key;
        spec.fine_tune_cents = wave.spec.fine_tune_cents;
        spec.target_sample_rate = wave.spec.target_sample_rate;
        spec.loop_mode = wave.spec.loop_mode;
        spec.loop_start_frame = wave.spec.loop_start_frame;
        spec.loop_length_frames = wave.spec.loop_length_frames;
        if (auto added = remember(insert_waveform_audio(*state, context, {partition, destination.volume_name, spec},
                                                        wave.audio, cancellation));
            !added)
            return std::unexpected(added.error());
    }
    for (const auto &sample : bundle.samples) {
        if (sample.interleaved_audio_path)
            return std::unexpected(transaction_error("Native authoring requires prepared audio, not source paths"));
        if (auto added =
                remember(insert_sbnk(*state, context, {partition, destination.volume_name, sample}, cancellation));
            !added)
            return std::unexpected(added.error());
    }
    for (const auto &bank : bundle.sample_banks)
        if (auto added =
                remember(insert_sbac(*state, context, {partition, destination.volume_name, bank}, cancellation));
            !added)
            return std::unexpected(added.error());
    for (const auto &program : bundle.programs)
        if (auto added =
                remember(insert_program(*state, context, {partition, destination.volume_name, program}, cancellation));
            !added)
            return std::unexpected(added.error());
    return finish_preparation(std::move(*state), std::move(path), policy, cancellation);
}

} // namespace axk::a_series::authoring
