#include "alteration_internal.hpp"

#include <algorithm>
#include <limits>
#include <ranges>

#include "axklib/object.hpp"
#include "axklib/sequence.hpp"

namespace axk::alteration_internal {
namespace {

constexpr std::uint64_t maximum_midi_bytes = 16U * 1024U * 1024U;

Result<std::vector<std::byte>> read_midi(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    if ((*reader)->size() == 0U || (*reader)->size() > maximum_midi_bytes ||
        (*reader)->size() > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{transaction_error("MIDI file size is outside the supported 1..16 MiB range")};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>((*reader)->size()));
    if (auto read = (*reader)->read_exact_at(0U, bytes, cancellation); !read)
        return std::unexpected{read.error()};
    return bytes;
}

OperationReport sequence_report(OperationContext context, PartitionIndex partition, std::string_view volume_name,
                                std::string_view object_name) {
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = partition;
    report.volume_name = volume_name;
    report.object_name = object_name;
    return report;
}

} // namespace

Result<OperationReport> delete_sequence(TransactionState &state, OperationContext context,
                                        const DeleteSequenceOperation &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("delete-sequence partition does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SEQU", operation.sequence_name, "SEQU", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded || std::get_if<CurrentSequence>(&decoded->payload) == nullptr)
        return std::unexpected{transaction_error("Sequence is unreadable")};
    if (auto removed = remove_directory_entry(state, partition, located->first, located->second,
                                              operation.sequence_name, cancellation);
        !removed) {
        return std::unexpected{removed.error()};
    }
    auto freed = release_record(partition, located->second);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == located->second || target == located->second);
    });
    auto report = sequence_report(context, *partition_index, operation.volume_name, operation.sequence_name);
    report.removed_sfs_ids = {located->second};
    report.freed_clusters = *freed;
    return report;
}

Result<OperationReport> insert_sequence(TransactionState &state, OperationContext context,
                                        const InsertSequenceOperation &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("insert-sequence partition does not exist")};
    auto &partition = found->second;
    auto directory = volume_category(state, partition, operation.volume_name, "SEQU", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto existing = category_objects(state, partition, operation.volume_name, "SEQU", ObjectType::sequ, cancellation);
    if (!existing)
        return std::unexpected{existing.error()};
    if (std::ranges::contains(*existing, operation.sequence.name, &CategoryObject::name))
        return std::unexpected{transaction_error("Sequence already exists")};
    auto midi = read_midi(operation.sequence.midi_path, cancellation);
    if (!midi)
        return std::unexpected{midi.error()};
    auto payload = smf0_to_current_sequence(*midi, operation.sequence.name, operation.sequence.system_exclusive_policy);
    if (!payload)
        return std::unexpected{payload.error()};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    if (auto appended = append_directory_entry(state, partition, *directory, allocated->first, operation.sequence.name,
                                               cancellation);
        !appended) {
        return std::unexpected{appended.error()};
    }
    auto report = sequence_report(context, *partition_index, operation.volume_name, operation.sequence.name);
    report.inserted_sfs_ids = {allocated->first};
    report.allocated_clusters = allocated->second;
    return report;
}

Result<OperationReport> rename_sequence(TransactionState &state, OperationContext context,
                                        const RenameSequenceOperation &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("rename-sequence partition does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SEQU", operation.sequence_name, "SEQU", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto sequences = category_objects(state, partition, operation.volume_name, "SEQU", ObjectType::sequ, cancellation);
    if (!sequences)
        return std::unexpected{sequences.error()};
    if (std::ranges::any_of(*sequences, [&](const CategoryObject &sequence) {
            return sequence.id != located->second && sequence.name == operation.new_sequence_name;
        })) {
        return std::unexpected{transaction_error("Sequence rename destination exists")};
    }
    if (auto renamed = rename_object_payload(state, partition, located->second, operation.sequence_name,
                                             operation.new_sequence_name, cancellation);
        !renamed) {
        return std::unexpected{renamed.error()};
    }
    if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                              operation.sequence_name, operation.new_sequence_name, cancellation);
        !renamed) {
        return std::unexpected{renamed.error()};
    }
    return sequence_report(context, *partition_index, operation.volume_name, operation.new_sequence_name);
}

} // namespace axk::alteration_internal
