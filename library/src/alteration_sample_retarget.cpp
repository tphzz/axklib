#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {
namespace {
Result<SfsId> retarget_member(TransactionState &state, MutablePartition &partition, std::string_view volume,
                              std::string_view name, const CurrentSbnkMember &member, bool right,
                              std::uint8_t loop_mode, std::vector<std::byte> &output,
                              const CancellationToken &cancellation) {
    auto located = category_object(state, partition, volume, "SMPL", name, "SMPL", cancellation);
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
        return std::unexpected{transaction_error("Retargeting requires current Wave Data")};
    if (auto control = validate_smpl_pcm_transfer_control(*wave); !control)
        return std::unexpected{control.error()};
    const auto width = wave->stored_sample_width_bytes.value;
    const std::uint64_t end = static_cast<std::uint64_t>(member.wave_start_frame) + member.wave_length_frames;
    const std::uint64_t loop_end = static_cast<std::uint64_t>(member.loop_start_frame) + member.loop_length_frames;
    if ((width != 1U && width != 2U) || wave->stored_segment_offset != 0U ||
        wave->stored_segment_bytes != wave->stored_pcm_bytes || wave->stored_pcm_offset < 0xacU ||
        wave->stored_pcm_offset > payload->size() ||
        wave->stored_pcm_bytes > payload->size() - wave->stored_pcm_offset || wave->stored_pcm_bytes % width != 0U ||
        wave->wave_data_reference_value.value == 0U || wave->sample_rate.value == 0U ||
        wave->sample_rate.value != wave->duplicate_sample_rate.value || member.root_key > 127U ||
        member.fine_tune_cents < -63 || member.fine_tune_cents > 63 || member.wave_length_frames == 0U ||
        end > wave->stored_pcm_bytes / width || end > maximum_wave_data_frames_per_channel || loop_mode > 5U ||
        (member.loop_length_frames == 0U ? (loop_mode == 1U || loop_mode == 2U || member.loop_start_frame != 0U)
                                         : (member.loop_start_frame < member.wave_start_frame || loop_end > end)))
        return std::unexpected{
            transaction_error("Retargeted Wave Data cannot contain the preserved Sample playback state")};
    ByteWriter writer{output};
    if (auto written = writer.write_ascii_field(right ? 0x88U : 0x78U, 16U, name); !written)
        return std::unexpected{written.error()};
    if (name != member.wave_data_name) {
        if (auto written = writer.write_be32(right ? 0x9cU : 0x98U, 0U); !written)
            return std::unexpected{written.error()};
    }
    if (auto written = writer.write_be32(right ? 0xa4U : 0xa0U, wave->wave_data_reference_value.value); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be16(right ? 0xdaU : 0xd8U, wave->sample_rate.value); !written)
        return std::unexpected{written.error()};
    if (auto written =
            writer.write_be16(right ? 0xe0U : 0xdeU, detail::sample_pitch_word(member.root_key, member.fine_tune_cents,
                                                                               wave->sample_rate.value));
        !written)
        return std::unexpected{written.error()};
    return located->second;
}
} // namespace

Result<OperationReport> retarget_sample_wave_data(TransactionState &state, OperationContext context,
                                                  const RetargetSampleWaveDataOperation &operation,
                                                  const CancellationToken &cancellation) {
    auto index = resolve_partition(state, operation.partition);
    if (!index)
        return std::unexpected{index.error()};
    const auto found = state.partitions.find(index->value);
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
    if (package_internal::hex_digest(package_internal::sha256(*payload)) != operation.expected_payload_sha256)
        return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                          "Sample no longer matches the expected payload")};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
    const ByteReader reader{*payload};
    const auto version = reader.be32(0x14U);
    if (!sample || !version || *version != 4U ||
        sample->right.has_value() != operation.right_waveform_name.has_value() ||
        (sample->right &&
         ((sample->sample_flags & 2U) != 0U || sample->right->wave_data_name == sample->left.wave_data_name)) ||
        (!sample->right && (sample->sample_flags & 2U) == 0U))
        return std::unexpected{
            transaction_error("Sample retargeting must preserve a supported current source topology")};
    auto updated = *payload;
    auto left = retarget_member(state, partition, operation.volume_name, operation.waveform_name, sample->left, false,
                                sample->loop_mode, updated, cancellation);
    if (!left)
        return std::unexpected{left.error()};
    std::optional<SfsId> right;
    if (sample->right) {
        auto target = retarget_member(state, partition, operation.volume_name, *operation.right_waveform_name,
                                      *sample->right, true, sample->loop_mode, updated, cancellation);
        if (!target)
            return std::unexpected{target.error()};
        const ByteReader changed{updated};
        if (changed.be16(0xd8U).value() != changed.be16(0xdaU).value())
            return std::unexpected{transaction_error("Stereo Sample retargeting requires matching Wave Data rates")};
        right = *target;
    }
    if (std::equal(payload->begin() + 0x6c, payload->begin() + 0x6f, payload->begin() + 0x78))
        std::copy_n(updated.begin() + 0x78, 3U, updated.begin() + 0x6c);
    if (auto replaced =
            replace_fixed_object_payload(state, partition, located->second, std::move(updated), cancellation);
        !replaced)
        return std::unexpected{replaced.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        return std::get<0>(edge) == *index && std::get<1>(edge) == located->second;
    });
    state.known_edges.emplace_back(*index, located->second, *left);
    if (right)
        state.known_edges.emplace_back(*index, located->second, *right);
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_name;
    return report;
}
} // namespace axk::alteration_internal
