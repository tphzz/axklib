#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

Result<OperationReport> delete_sbnk(TransactionState &state, OperationContext context,
                                    const DeleteSampleOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("partition index does not exist")};
    }
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SBNK", operation.sample_name, "SBNK", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    const auto [directory_id, sample_id] = *located;
    auto payload = current_payload(state, partition, sample_id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
    if (sample == nullptr) {
        return std::unexpected{transaction_error("Sample is not a current SBNK object")};
    }
    if (!sample->linked_program_numbers.empty()) {
        return std::unexpected{transaction_error("Sample is referenced by its Program link bitmap")};
    }
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == sample_id && record_exists(partition, source)) {
            return std::unexpected{transaction_error("Sample is referenced by a Program or Sample Bank")};
        }
    }
    if (auto removed =
            remove_directory_entry(state, partition, directory_id, sample_id, operation.sample_name, cancellation);
        !removed) {
        return std::unexpected{removed.error()};
    }
    auto freed = release_record(partition, sample_id);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == sample_id || target == sample_id);
    });
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_name;
    report.removed_sfs_ids = {sample_id};
    report.freed_clusters = *freed;
    return report;
}

Result<detail::PreparedWaveformMember> waveform_member(TransactionState &state, MutablePartition &partition,
                                                       std::string_view volume_name, std::string_view waveform_name,
                                                       const CancellationToken &cancellation) {
    auto located = category_object(state, partition, volume_name, "SMPL", waveform_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *wave_data = std::get_if<CurrentSmpl>(&decoded->payload);
    if (wave_data == nullptr || wave_data->wave_data_reference_value.value == 0U) {
        return std::unexpected{transaction_error("waveform has no usable current SMPL reference value")};
    }
    return detail::PreparedWaveformMember{std::string{waveform_name}, wave_data->wave_data_reference_value.value,
                                          wave_data->duplicate_sample_rate.value, wave_data->wave_length_frames.value};
}

Result<OperationReport> insert_sbnk(TransactionState &state, OperationContext context,
                                    const InsertSampleOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("insert-sbnk target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = operation.sample;
    auto directory = volume_category(state, partition, operation.volume_name, "SBNK", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto directory_payload = current_payload(state, partition, *directory, cancellation);
    if (!directory_payload)
        return std::unexpected{directory_payload.error()};
    auto entries = parse_directory(*directory_payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const ParsedDirectoryEntry &entry) { return entry.name == spec.name; })) {
        return std::unexpected{transaction_error("volume already contains the requested Sample")};
    }
    if (!spec.waveform_id) {
        return std::unexpected{transaction_error("Sample requires waveform_name")};
    }
    auto left = waveform_member(state, partition, operation.volume_name, *spec.waveform_id, cancellation);
    if (!left)
        return std::unexpected{left.error()};
    std::optional<detail::PreparedWaveformMember> right;
    if (spec.right_waveform_id) {
        auto member = waveform_member(state, partition, operation.volume_name, *spec.right_waveform_id, cancellation);
        if (!member)
            return std::unexpected{member.error()};
        if (member->sample_rate != left->sample_rate || member->frame_count != left->frame_count) {
            return std::unexpected{transaction_error("stereo Sample requires matching Wave Data sample "
                                                     "rates and frame counts")};
        }
        right = std::move(*member);
    }
    auto payload = detail::prepare_sbnk_payload(spec, *left, right);
    if (!payload)
        return std::unexpected{payload.error()};
    // The alteration contract preserves the complete current SBNK contract
    // window, while fresh-image records use the sampler's shorter object size.
    payload->resize(0x200U, std::byte{0});
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    const auto [bank_id, cluster_count] = *allocated;
    if (auto appended = append_directory_entry(state, partition, *directory, bank_id, spec.name, cancellation);
        !appended) {
        return std::unexpected{appended.error()};
    }
    const auto left_object =
        category_object(state, partition, operation.volume_name, "SMPL", *spec.waveform_id, "SMPL", cancellation);
    if (!left_object)
        return std::unexpected{left_object.error()};
    state.known_edges.emplace_back(*partition_index, bank_id, left_object->second);
    if (spec.right_waveform_id) {
        const auto right_object = category_object(state, partition, operation.volume_name, "SMPL",
                                                  *spec.right_waveform_id, "SMPL", cancellation);
        if (!right_object)
            return std::unexpected{right_object.error()};
        state.known_edges.emplace_back(*partition_index, bank_id, right_object->second);
    }
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = spec.name;
    report.inserted_sfs_ids = {bank_id};
    report.allocated_clusters = cluster_count;
    return report;
}

Result<OperationReport> insert_waveform(TransactionState &state, OperationContext context,
                                        const InsertWaveformOperation &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("insert-waveform target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = operation.waveform;
    auto directory = volume_category(state, partition, operation.volume_name, "SMPL", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto directory_payload = current_payload(state, partition, *directory, cancellation);
    if (!directory_payload)
        return std::unexpected{directory_payload.error()};
    auto entries = parse_directory(*directory_payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};

    std::set<std::uint32_t> link_ids;
    for (const auto &entry : *entries) {
        if (entry.name == "." || entry.name == "..")
            continue;
        if (std::ranges::contains(spec.waveform_names, entry.name)) {
            return std::unexpected{transaction_error("volume already contains a requested waveform name")};
        }
        auto payload = current_payload(state, partition, entry.id, cancellation);
        if (!payload)
            return std::unexpected{transaction_error("existing SMPL record is unresolved; link-ID "
                                                     "allocation is unsafe")};
        auto decoded = decode_object(*payload);
        if (!decoded)
            return std::unexpected{decoded.error()};
        const auto *wave_data = std::get_if<CurrentSmpl>(&decoded->payload);
        if (wave_data == nullptr || wave_data->wave_data_reference_value.value == 0U) {
            return std::unexpected{transaction_error("existing waveform has no current SMPL reference value")};
        }
        link_ids.insert(wave_data->wave_data_reference_value.value);
    }

    AudioImportOptions options;
    options.expected_channels = static_cast<std::uint8_t>(spec.waveform_names.size());
    options.target_sample_rate = spec.target_sample_rate;
    auto audio = import_sampler_audio(spec.path, options);
    if (!audio)
        return std::unexpected{audio.error()};
    std::uint32_t candidate = 0x016b1dbcU;
    std::vector<SfsId> inserted;
    std::uint64_t allocated_clusters{};
    for (std::size_t channel = 0; channel < spec.waveform_names.size(); ++channel) {
        while (link_ids.contains(candidate))
            candidate += 0x100U;
        const auto link_id = candidate;
        link_ids.insert(link_id);
        candidate += 0x100U;

        auto mono = *audio;
        mono.source_channels = 1U;
        mono.pcm_channels = {audio->pcm_channels[channel]};
        WaveformSpec waveform;
        waveform.id = spec.waveform_names[channel];
        waveform.name = spec.waveform_names[channel];
        waveform.path = spec.path;
        waveform.root_key = spec.root_key;
        waveform.target_sample_rate = spec.target_sample_rate;
        auto payload = detail::prepare_smpl_payload(waveform, mono, link_id);
        if (!payload)
            return std::unexpected{payload.error()};
        auto stored = allocate_record(partition, std::move(*payload), PayloadKind::object);
        if (!stored)
            return std::unexpected{stored.error()};
        if (auto appended = append_directory_entry(state, partition, *directory, stored->first,
                                                   spec.waveform_names[channel], cancellation);
            !appended) {
            return std::unexpected{appended.error()};
        }
        inserted.push_back(stored->first);
        allocated_clusters += stored->second;
    }
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    for (std::size_t index = 0; index < spec.waveform_names.size(); ++index) {
        if (index != 0U)
            report.object_name += ';';
        report.object_name += spec.waveform_names[index];
    }
    report.inserted_sfs_ids = std::move(inserted);
    report.allocated_clusters = allocated_clusters;
    report.audio_import = AudioImportSummary{audio->source_path,
                                             audio->source_format,
                                             audio->source_subtype,
                                             audio->source_channels,
                                             audio->source_sample_rate,
                                             audio->output_sample_rate,
                                             audio->source_sample_width_bits,
                                             audio->output_sample_width_bits,
                                             audio->output_frames,
                                             audio->resampled,
                                             audio->quantized,
                                             audio->sample_width_converted,
                                             audio->source_channels == 2U,
                                             audio->dither_algorithm,
                                             audio->clipped_samples};
    return report;
}

Result<OperationReport> delete_waveform(TransactionState &state, OperationContext context,
                                        const DeleteWaveformOperation &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("partition index does not exist")};
    }
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SMPL", operation.waveform_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    const auto [directory_id, waveform_id] = *located;
    auto waveform_payload = current_payload(state, partition, waveform_id, cancellation);
    if (!waveform_payload)
        return std::unexpected{waveform_payload.error()};
    auto waveform_object = decode_object(*waveform_payload);
    if (!waveform_object)
        return std::unexpected{waveform_object.error()};
    const auto *wave_data = std::get_if<CurrentSmpl>(&waveform_object->payload);
    if (wave_data == nullptr || wave_data->wave_data_reference_value.value == 0U) {
        return std::unexpected{transaction_error("waveform cannot be classified as known_unreferenced")};
    }

    auto sample_directory = volume_category(state, partition, operation.volume_name, "SBNK", cancellation);
    if (!sample_directory)
        return std::unexpected{sample_directory.error()};
    auto sample_directory_payload = current_payload(state, partition, *sample_directory, cancellation);
    if (!sample_directory_payload)
        return std::unexpected{sample_directory_payload.error()};
    auto sample_entries = parse_directory(*sample_directory_payload, *sample_directory);
    if (!sample_entries)
        return std::unexpected{sample_entries.error()};
    for (const auto &entry : *sample_entries) {
        if (entry.name == "." || entry.name == "..")
            continue;
        auto payload = current_payload(state, partition, entry.id, cancellation);
        if (!payload) {
            return std::unexpected{transaction_error("waveform ownership is ambiguous because an "
                                                     "SBNK is unreadable")};
        }
        auto object = decode_object(*payload);
        if (!object)
            return std::unexpected{object.error()};
        const auto *sample = std::get_if<CurrentSbnk>(&object->payload);
        if (sample == nullptr) {
            return std::unexpected{transaction_error("waveform ownership is ambiguous because an "
                                                     "SBNK entry is unresolved")};
        }
        const auto references = [&](const CurrentSbnkMember &member) {
            return member.wave_data_name == operation.waveform_name;
        };
        if (references(sample->left) || (sample->right && references(*sample->right))) {
            return std::unexpected{transaction_error("waveform is referenced, not known_unreferenced")};
        }
    }
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        static_cast<void>(source);
        if (edge_partition == *partition_index && target == waveform_id) {
            return std::unexpected{transaction_error("waveform has a known incoming reference, "
                                                     "not known_unreferenced")};
        }
    }
    if (auto removed =
            remove_directory_entry(state, partition, directory_id, waveform_id, operation.waveform_name, cancellation);
        !removed) {
        return std::unexpected{removed.error()};
    }
    auto freed = release_record(partition, waveform_id);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == waveform_id || target == waveform_id);
    });
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.waveform_name;
    report.removed_sfs_ids = {waveform_id};
    report.freed_clusters = *freed;
    return report;
}

void put_padded_name(std::span<std::byte> payload, std::size_t offset, std::string_view name) {
    std::fill(payload.begin() + static_cast<std::ptrdiff_t>(offset),
              payload.begin() + static_cast<std::ptrdiff_t>(offset + 16U), std::byte{' '});
    std::ranges::transform(name, payload.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char value) { return static_cast<std::byte>(value); });
}

Result<OperationReport> rename_waveform(TransactionState &state, OperationContext context,
                                        const RenameWaveformOperation &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SMPL", operation.waveform_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto waveform_payload = current_payload(state, partition, located->second, cancellation);
    if (!waveform_payload)
        return std::unexpected{waveform_payload.error()};
    auto waveform_object = decode_object(*waveform_payload);
    if (!waveform_object)
        return std::unexpected{waveform_object.error()};
    const auto *wave_data = std::get_if<CurrentSmpl>(&waveform_object->payload);
    if (wave_data == nullptr || wave_data->wave_data_reference_value.value == 0U)
        return std::unexpected{transaction_error("waveform has no current reference value")};
    auto waveforms = category_objects(state, partition, operation.volume_name, "SMPL", ObjectType::smpl, cancellation);
    if (!waveforms)
        return std::unexpected{waveforms.error()};
    for (const auto &other : *waveforms) {
        if (other.id == located->second)
            continue;
        if (other.name == operation.new_waveform_name) {
            return std::unexpected{transaction_error("waveform rename target name is not unique")};
        }
    }
    auto samples = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk, cancellation);
    if (!samples)
        return std::unexpected{samples.error()};
    std::set<SfsId> updated_samples;
    for (const auto &sample_row : *samples) {
        const auto *sample = std::get_if<CurrentSbnk>(&sample_row.decoded.payload);
        std::vector<std::pair<std::size_t, std::size_t>> offsets;
        const auto inspect = [&](const CurrentSbnkMember &member, std::size_t name_offset, std::size_t cache_offset) {
            if (member.wave_data_name == operation.waveform_name)
                offsets.emplace_back(name_offset, cache_offset);
        };
        inspect(sample->left, 0x78U, 0xa0U);
        if (sample->right)
            inspect(*sample->right, 0x88U, 0xa4U);
        if (!offsets.empty()) {
            auto payload = sample_row.payload;
            ByteWriter writer{payload};
            for (const auto &[name_offset, cache_offset] : offsets) {
                put_padded_name(payload, name_offset, operation.new_waveform_name);
                if (auto written = writer.write_be32(cache_offset, wave_data->wave_data_reference_value.value);
                    !written) {
                    return std::unexpected{written.error()};
                }
            }
            if (auto replaced =
                    replace_fixed_object_payload(state, partition, sample_row.id, std::move(payload), cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            updated_samples.insert(sample_row.id);
        }
    }
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == located->second && !updated_samples.contains(source))
            return std::unexpected{transaction_error("known waveform references exceed exact rename set")};
    }
    if (auto renamed = rename_object_payload(state, partition, located->second, operation.waveform_name,
                                             operation.new_waveform_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                              operation.waveform_name, operation.new_waveform_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_waveform_name;
    return report;
}

Result<OperationReport> rename_sbnk(TransactionState &state, OperationContext context,
                                    const RenameSampleOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SBNK", operation.sample_name, "SBNK", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto samples = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk, cancellation);
    if (!samples)
        return std::unexpected{samples.error()};
    if (std::ranges::any_of(*samples, [&](const auto &sample) {
            return sample.id != located->second && sample.name == operation.new_sample_name;
        }))
        return std::unexpected{transaction_error("SBNK rename destination exists")};
    auto sample_payload = current_payload(state, partition, located->second, cancellation);
    if (!sample_payload)
        return std::unexpected{sample_payload.error()};
    auto sample_object = decode_object(*sample_payload);
    if (!sample_object)
        return std::unexpected{sample_object.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&sample_object->payload);
    if (sample == nullptr)
        return std::unexpected{transaction_error("SBNK is unreadable")};
    const auto banked = (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) != 0U;

    auto sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!sample_banks)
        return std::unexpected{sample_banks.error()};
    std::set<SfsId> sample_bank_references;
    for (const auto &sample_bank_row : *sample_banks) {
        const auto *sample_bank = std::get_if<CurrentSbac>(&sample_bank_row.decoded.payload);
        auto payload = sample_bank_row.payload;
        bool changed{};
        for (const auto &slot : sample_bank->slots) {
            if (slot.name == operation.new_sample_name)
                return std::unexpected{transaction_error("SBAC already references SBNK rename destination")};
            if (slot.name != operation.sample_name)
                continue;
            if (slot.raw_handle != 0U)
                return std::unexpected{transaction_error("SBAC member has unsupported nonzero handle")};
            put_padded_name(payload, slot.offset, operation.new_sample_name);
            changed = true;
        }
        if (changed) {
            if (auto replaced = replace_fixed_object_payload(state, partition, sample_bank_row.id, std::move(payload),
                                                             cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            sample_bank_references.insert(sample_bank_row.id);
        }
    }
    if (sample_bank_references.size() != (banked ? 1U : 0U))
        return std::unexpected{transaction_error("Sample membership flag disagrees with exact Sample Bank membership")};

    auto programs = category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    std::set<SfsId> program_references;
    std::set<std::uint8_t> direct_numbers;
    for (const auto &program_row : *programs) {
        int number{};
        const auto parsed_number =
            std::from_chars(program_row.name.data(), program_row.name.data() + program_row.name.size(), number);
        if (parsed_number.ec != std::errc{} || parsed_number.ptr != program_row.name.data() + program_row.name.size()) {
            return std::unexpected{transaction_error("Program slot name is unsupported")};
        }
        if (number < 1 || number > 128 || std::format("{:03}", number) != program_row.name)
            return std::unexpected{transaction_error("Program slot name is unsupported")};
        const auto *program = std::get_if<CurrentProg>(&program_row.decoded.payload);
        auto payload = program_row.payload;
        bool changed{};
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (assignment.kind != 0x10U)
                continue;
            if (assignment.name == operation.new_sample_name)
                return std::unexpected{transaction_error("Program already assigns SBNK rename destination")};
            if (assignment.name != operation.sample_name)
                continue;
            if (assignment.raw_handle != 0U)
                return std::unexpected{transaction_error("Program assignment has unsupported nonzero handle")};
            put_padded_name(payload, 0x120U + index * 0x38U, operation.new_sample_name);
            changed = true;
        }
        if (changed) {
            if (auto replaced =
                    replace_fixed_object_payload(state, partition, program_row.id, std::move(payload), cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            program_references.insert(program_row.id);
            direct_numbers.insert(static_cast<std::uint8_t>(number));
        }
    }
    std::set<std::uint8_t> bitmap_numbers(sample->linked_program_numbers.begin(), sample->linked_program_numbers.end());
    if (bitmap_numbers != direct_numbers)
        return std::unexpected{transaction_error("SBNK Program bitmap disagrees with exact Program assignments")};
    std::set<SfsId> expected = sample_bank_references;
    expected.insert(program_references.begin(), program_references.end());
    std::set<SfsId> known_incoming;
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == located->second) {
            known_incoming.insert(source);
        }
    }
    if (known_incoming != expected) {
        return std::unexpected{transaction_error("SBNK raw references disagree with known edges")};
    }
    if (auto renamed = rename_object_payload(state, partition, located->second, operation.sample_name,
                                             operation.new_sample_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    if (auto renamed = rename_directory_entry(state, partition, located->first, located->second, operation.sample_name,
                                              operation.new_sample_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_sample_name;
    return report;
}

} // namespace axk::alteration_internal
