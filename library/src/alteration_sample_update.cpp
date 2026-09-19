#include "alteration_internal.hpp"

#include <cstdint>
#include <span>

#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {
namespace {
Result<void> validate_window_source(TransactionState &state, MutablePartition &partition, std::string_view volume,
                                    const CurrentSbnkMember &member, const SamplePlaybackWindow &window,
                                    const CancellationToken &cancellation) {
    auto located = category_object(state, partition, volume, "SMPL", member.wave_data_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *wave = std::get_if<CurrentSmpl>(&decoded->payload);
    if (!wave)
        return std::unexpected{transaction_error("Playback editing requires current Wave Data")};
    if (auto valid = validate_smpl_pcm_transfer_control(*wave); !valid)
        return std::unexpected{valid.error()};
    const auto width = wave->stored_sample_width_bytes.value;
    const auto end = static_cast<std::uint64_t>(window.start_frame) + window.length_frames;
    if ((width != 1U && width != 2U) || wave->stored_segment_offset != 0U ||
        wave->stored_segment_bytes != wave->stored_pcm_bytes || wave->stored_pcm_offset < 0xacU ||
        wave->stored_pcm_offset > payload->size() ||
        wave->stored_pcm_bytes > payload->size() - wave->stored_pcm_offset || wave->stored_pcm_bytes % width != 0U ||
        wave->sample_rate.value == 0U || wave->sample_rate.value != wave->duplicate_sample_rate.value ||
        member.sample_rate != wave->sample_rate.value || window.length_frames == 0U ||
        end > wave->stored_pcm_bytes / width || end > maximum_wave_data_frames_per_channel)
        return std::unexpected{transaction_error("Playback window requires complete, matching and bounded Wave Data")};
    return {};
}

Result<void> set_window(TransactionState &state, MutablePartition &partition,
                        const UpdateSampleParametersOperation &operation, std::vector<std::byte> &payload,
                        const CancellationToken &cancellation) {
    const auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
    const ByteReader reader{payload};
    const auto selector = reader.be32(0x14U).value_or(0U);
    if (!sample || payload.size() < 0x164U || (selector != 1U && selector != 2U && selector != 4U) ||
        (sample->right &&
         ((sample->sample_flags & 2U) != 0U || sample->right->wave_data_name == sample->left.wave_data_name)) ||
        (!sample->right && (sample->sample_flags & 2U) == 0U))
        return std::unexpected{transaction_error("Playback editing requires a supported current Sample topology")};
    const auto &window = *operation.playback_window;
    if (auto valid =
            validate_window_source(state, partition, operation.volume_name, sample->left, window, cancellation);
        !valid)
        return valid;
    if (sample->right) {
        if (sample->left.sample_rate != sample->right->sample_rate)
            return std::unexpected{transaction_error("Stereo playback editing requires matching rates")};
        if (auto valid =
                validate_window_source(state, partition, operation.volume_name, *sample->right, window, cancellation);
            !valid)
            return valid;
    }
    ByteWriter writer{payload};
    if (auto result = writer.write_be32(0xe8U, window.start_frame); !result)
        return result;
    if (auto result = writer.write_be32(0xf0U, window.length_frames); !result)
        return result;
    if (sample->right) {
        if (auto result = writer.write_be32(0xecU, window.start_frame); !result)
            return result;
        if (auto result = writer.write_be32(0xf4U, window.length_frames); !result)
            return result;
    }
    return writer.write_be32(0x15cU, window.start_frame + window.length_frames);
}
} // namespace

Result<OperationReport> update_sbnk_parameters(TransactionState &state, OperationContext context,
                                               const UpdateSampleParametersOperation &operation,
                                               const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SBNK", operation.sample_name, "SBNK", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (operation.expected_payload_sha256 &&
        package_internal::hex_digest(package_internal::sha256(*payload)) != *operation.expected_payload_sha256)
        return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                          "Sample no longer matches the edited baseline")};
    auto parameters = operation.parameters;
    if (operation.playback_window) {
        if (auto updated = set_window(state, partition, operation, *payload, cancellation); !updated)
            return std::unexpected{updated.error()};
        // Validate retained as well as changed loops against the candidate window.
        if (!parameters.loop_mode)
            parameters.loop_mode = static_cast<AudioSamplerLoopMode>(std::to_integer<std::uint8_t>((*payload)[0xe5U]));
    }
    if (auto updated = detail::apply_sample_parameters_to_payload(*payload, parameters); !updated)
        return std::unexpected{updated.error()};
    if (auto replaced =
            replace_fixed_object_payload(state, partition, located->second, std::move(*payload), cancellation);
        !replaced)
        return std::unexpected{replaced.error()};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_name;
    return report;
}
} // namespace axk::alteration_internal
