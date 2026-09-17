#include "alteration_internal.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <utility>

#include "alteration_manifest_program.hpp"
#include "axklib/package_archive.hpp"

namespace axk::alteration_internal {

namespace {
Result<std::set<SfsId>> assignment_targets(TransactionState &state, MutablePartition &partition,
                                           std::string_view volume, std::span<const std::byte> payload,
                                           const CancellationToken &cancellation) {
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *program = std::get_if<CurrentProg>(&decoded->payload);
    if (!program)
        return std::unexpected{transaction_error("Program is unreadable")};
    std::set<SfsId> result;
    for (const auto &row : program->assignments) {
        if (row.name.empty() && row.kind == 0U)
            continue;
        if (row.name.empty() || (row.kind != 0x10U && row.kind != 0x11U))
            return std::unexpected{transaction_error("Program assignment target is unsupported")};
        const auto category = row.kind == 0x10U ? "SBNK" : "SBAC";
        auto target = category_object(state, partition, volume, category, row.name, category, cancellation);
        if (!target)
            return std::unexpected{target.error()};
        result.insert(target->second);
    }
    return result;
}
} // namespace

Result<OperationReport> replace_program_assignments(TransactionState &state, OperationContext context,
                                                    const ReplaceProgramAssignmentsOperation &operation,
                                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    const auto name = std::format("{:03}", operation.program_number);
    auto located = category_object(state, partition, operation.volume_name, "PROG", name, "PROG", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (package_internal::hex_digest(package_internal::sha256(*payload)) != operation.expected_payload_sha256)
        return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                          "Program no longer matches the expected payload")};
    auto replacement = detail::replace_prog_assignment_rows(*payload, operation);
    if (!replacement)
        return std::unexpected{replacement.error()};
    auto old_targets = assignment_targets(state, partition, operation.volume_name, *payload, cancellation);
    if (!old_targets)
        return std::unexpected{old_targets.error()};
    auto new_targets = assignment_targets(state, partition, operation.volume_name, *replacement, cancellation);
    if (!new_targets)
        return std::unexpected{new_targets.error()};
    for (const auto type : {ObjectType::sbnk, ObjectType::sbac}) {
        const bool sample = type == ObjectType::sbnk;
        auto objects =
            category_objects(state, partition, operation.volume_name, sample ? "SBNK" : "SBAC", type, cancellation);
        if (!objects)
            return std::unexpected{objects.error()};
        for (const auto &object : *objects) {
            auto bit = sample ? sbnk_program_bit(object.payload, operation.program_number)
                              : sbac_program_bit(object.payload, operation.program_number);
            if (!bit)
                return std::unexpected{bit.error()};
            if (*bit != old_targets->contains(object.id))
                return std::unexpected{
                    transaction_error("Program assignments disagree with target Program-link bitmaps")};
            const auto selected = new_targets->contains(object.id);
            if (selected == *bit)
                continue;
            auto updated = sample ? set_sbnk_program_bit(state, partition, object.id, operation.program_number,
                                                         selected, cancellation)
                                  : set_sbac_program_bit(state, partition, object.id, operation.program_number,
                                                         selected, cancellation);
            if (!updated)
                return std::unexpected{updated.error()};
        }
    }
    auto growth = grow_record_capacity(state, partition, located->second, replacement->size(), cancellation);
    if (!growth)
        return std::unexpected{growth.error()};
    if (auto replaced =
            replace_record_payload(state, partition, located->second, std::move(*replacement), cancellation);
        !replaced)
        return std::unexpected{replaced.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        return std::get<0>(edge) == *partition_index && std::get<1>(edge) == located->second;
    });
    for (const auto target : *new_targets)
        state.known_edges.emplace_back(*partition_index, located->second, target);
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = name;
    report.allocated_clusters = growth->first + growth->second;
    return report;
}

} // namespace axk::alteration_internal
