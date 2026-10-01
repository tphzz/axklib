#include "alteration_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/package_archive.hpp"

namespace axk::alteration_internal {
namespace {
bool same_name(std::string_view left, std::string_view right) {
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return std::ranges::equal(left, right, [&](char a, char b) {
        return fold(static_cast<unsigned char>(a)) == fold(static_cast<unsigned char>(b));
    });
}

Result<void> require_unused_name(TransactionState &state, MutablePartition &partition,
                                 const DuplicateSampleOperation &operation, const CancellationToken &cancellation) {
    auto samples = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk, cancellation);
    if (!samples)
        return std::unexpected{samples.error()};
    if (std::ranges::any_of(*samples, [&](const auto &row) { return same_name(row.name, operation.new_name); }))
        return std::unexpected{transaction_error("volume already contains the requested Sample name")};
    auto banks = category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!banks)
        return std::unexpected{banks.error()};
    for (const auto &row : *banks)
        for (const auto &slot : std::get<CurrentSbac>(row.decoded.payload).slots)
            if (slot.active && same_name(slot.name, operation.new_name))
                return std::unexpected{transaction_error("Sample Bank already references the requested Sample name")};
    auto programs = category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    for (const auto &row : *programs)
        for (const auto &assignment : std::get<CurrentProg>(row.decoded.payload).assignments)
            if (assignment.kind == 0x10U && same_name(assignment.name, operation.new_name))
                return std::unexpected{transaction_error("Program already assigns the requested Sample name")};
    return {};
}
} // namespace

Result<OperationReport> duplicate_sbnk(TransactionState &state, OperationContext context,
                                       const DuplicateSampleOperation &operation,
                                       const CancellationToken &cancellation) {
    auto index = resolve_partition(state, operation.partition);
    if (!index)
        return std::unexpected{index.error()};
    const auto found = state.partitions.find(index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto source =
        category_object(state, partition, operation.volume_name, "SBNK", operation.sample_name, "SBNK", cancellation);
    if (!source)
        return std::unexpected{source.error()};
    auto payload = current_payload(state, partition, source->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (operation.expected_payload_sha256 &&
        package_internal::hex_digest(package_internal::sha256(*payload)) != *operation.expected_payload_sha256)
        return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                          "Sample no longer matches the duplication baseline")};
    if (auto valid = require_unused_name(state, partition, operation, cancellation); !valid)
        return std::unexpected{valid.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
    if (!sample || payload->size() < 0x164U ||
        (sample->right && (sample->left.wave_start_frame != sample->right->wave_start_frame ||
                           sample->left.wave_length_frames != sample->right->wave_length_frames)))
        return std::unexpected{transaction_error("Duplication requires a supported current Sample playback layout")};
    UpdateSampleParametersOperation edit{operation.partition, operation.volume_name, operation.sample_name,
                                         operation.parameters, operation.playback_window};
    // Validate the complete candidate playback state even when no window is edited,
    // without normalizing bytes that the caller did not request to change.
    auto validated = *payload;
    auto validation = edit;
    if (!validation.playback_window)
        validation.playback_window =
            SamplePlaybackWindow{sample->left.wave_start_frame, sample->left.wave_length_frames};
    if (auto valid = apply_sample_edit(state, partition, validation, validated, cancellation); !valid)
        return std::unexpected{valid.error()};
    if (auto updated = apply_sample_edit(state, partition, edit, *payload, cancellation); !updated)
        return std::unexpected{updated.error()};
    std::vector<SfsId> waves;
    for (const auto &name :
         {sample->left.wave_data_name, sample->right ? sample->right->wave_data_name : std::string{}}) {
        if (name.empty())
            continue;
        auto wave = category_object(state, partition, operation.volume_name, "SMPL", name, "SMPL", cancellation);
        if (!wave)
            return std::unexpected{wave.error()};
        waves.push_back(wave->second);
    }
    // A new standalone Sample has no direct Program links or Sample Bank owner.
    std::fill(payload->begin() + 0xc0, payload->begin() + 0xd0, std::byte{0});
    (*payload)[0xd0U] &= std::byte{0xfe};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    const auto [id, clusters] = *allocated;
    if (auto renamed =
            rename_object_payload(state, partition, id, operation.sample_name, operation.new_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    if (auto appended = append_directory_entry(state, partition, source->first, id, operation.new_name, cancellation);
        !appended)
        return std::unexpected{appended.error()};
    for (const auto wave : waves)
        state.known_edges.emplace_back(*index, id, wave);
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_name;
    report.inserted_sfs_ids = {id};
    report.allocated_clusters = clusters;
    return report;
}
} // namespace axk::alteration_internal
