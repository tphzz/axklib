#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
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

Error transaction_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Error stale_transaction_error(std::string message) {
    return make_error(ErrorCode::transaction_stale, ErrorCategory::transaction, std::move(message));
}

Result<void> require_distinct_source_and_output(const std::filesystem::path &source,
                                                const std::filesystem::path &output, std::string_view operation) {
    std::error_code error;
    const auto canonical_source = std::filesystem::canonical(source, error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify source image")};
    }
    const auto canonical_output = std::filesystem::weakly_canonical(output, error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify output image")};
    }
    if (canonical_source == canonical_output) {
        return std::unexpected{transaction_error(std::string{operation} + " output must differ from source")};
    }
    const auto output_exists = std::filesystem::exists(output, error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not inspect output image identity")};
    }
    if (output_exists && std::filesystem::equivalent(source, output, error)) {
        return std::unexpected{transaction_error(std::string{operation} + " output must differ from source")};
    }
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not compare image identities")};
    }
    return {};
}

bool requires_object_graph(const AlterationManifest &manifest) {
    return std::ranges::any_of(manifest.operations, [](const AlterationOperation &operation) {
        return !std::holds_alternative<InsertVolumeOperation>(operation.data) &&
               !std::holds_alternative<RenameVolumeOperation>(operation.data) &&
               !std::holds_alternative<RenamePartitionOperation>(operation.data);
    });
}

const IndexRecord *record(const Partition &partition, SfsId id) {
    const auto found = std::ranges::find(partition.records, id, &IndexRecord::sfs_id);
    return found == partition.records.end() ? nullptr : &*found;
}

const MutablePartition::InsertedRecord *current_record(const MutablePartition &partition, SfsId id) {
    if (partition.deleted.contains(id))
        return nullptr;
    if (const auto found = partition.inserted.find(id); found != partition.inserted.end()) {
        return &found->second;
    }
    if (const auto found = partition.changed.find(id); found != partition.changed.end()) {
        return &found->second;
    }
    return nullptr;
}

bool record_exists(const MutablePartition &partition, SfsId id) {
    return current_record(partition, id) != nullptr ||
           (!partition.deleted.contains(id) && record(*partition.source, id) != nullptr);
}

Result<std::vector<std::byte>> current_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                               const CancellationToken &cancellation) {
    if (id.value == 1U && partition.root_payload)
        return *partition.root_payload;
    if (const auto *item = current_record(partition, id); item != nullptr)
        return item->payload;
    if (partition.deleted.contains(id) || record(*partition.source, id) == nullptr) {
        return std::unexpected{transaction_error("SFS record does not exist in transaction state")};
    }
    return state.container.read_record_data(partition.source->index, id, 64U * 1024U * 1024U, cancellation);
}

PayloadKind current_payload_kind(const MutablePartition &partition, SfsId id) {
    if (const auto *item = current_record(partition, id); item != nullptr) {
        return item->payload_kind;
    }
    const auto *source = record(*partition.source, id);
    return source == nullptr ? PayloadKind::unknown : source->payload_kind;
}

std::string directory_name(std::span<const std::byte> bytes) {
    if (bytes.size() < 32U)
        return {};
    const auto declared = static_cast<std::size_t>((std::to_integer<std::uint16_t>(bytes[2]) << 8U) |
                                                   std::to_integer<std::uint16_t>(bytes[3]));
    const auto count = std::min<std::size_t>(declared, 24U);
    std::string result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[8U + index]);
        if (value == 0U)
            break;
        result.push_back(static_cast<char>(value));
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

Result<std::vector<ParsedDirectoryEntry>> parse_directory(std::span<const std::byte> payload, SfsId id) {
    std::vector<ParsedDirectoryEntry> result;
    for (std::size_t offset = 0; offset + 32U <= payload.size(); offset += 32U) {
        const auto row = payload.subspan(offset, 32U);
        if (std::ranges::all_of(row.first(8U), [](std::byte value) { return value == std::byte{0}; })) {
            break;
        }
        const auto link = (std::to_integer<std::uint32_t>(row[4]) << 24U) |
                          (std::to_integer<std::uint32_t>(row[5]) << 16U) |
                          (std::to_integer<std::uint32_t>(row[6]) << 8U) | std::to_integer<std::uint32_t>(row[7]);
        result.push_back({SfsId{link}, directory_name(row), offset});
    }
    if (result.empty() || result.front().name != ".") {
        return std::unexpected{
            transaction_error("SFS ID " + std::to_string(id.value) + " is not a readable directory")};
    }
    return result;
}

Result<std::vector<std::byte>> read_raw(const RandomAccessReader &reader, std::uint64_t offset, std::size_t size) {
    std::vector<std::byte> result(size);
    if (auto read = reader.read_exact_at(offset, result); !read)
        return std::unexpected{read.error()};
    return result;
}

Result<std::vector<std::byte>> read_raw(const std::filesystem::path &path, std::uint64_t offset, std::size_t size) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return read_raw(**reader, offset, size);
}

void set_bitmap(std::vector<std::byte> &bitmap, std::uint32_t cluster, bool used) {
    const auto mask = static_cast<std::uint8_t>(0x80U >> (cluster % 8U));
    auto value = std::to_integer<std::uint8_t>(bitmap[cluster / 8U]);
    value = used ? static_cast<std::uint8_t>(value | mask)
                 : static_cast<std::uint8_t>(value & static_cast<std::uint8_t>(~mask));
    bitmap[cluster / 8U] = static_cast<std::byte>(value);
}

bool bitmap_used(const std::vector<std::byte> &bitmap, std::uint32_t cluster) {
    return (std::to_integer<std::uint8_t>(bitmap[cluster / 8U]) & (0x80U >> (cluster % 8U))) != 0U;
}

Result<std::vector<std::byte>> current_root_payload(TransactionState &state, MutablePartition &partition,
                                                    const CancellationToken &cancellation) {
    if (partition.root_payload)
        return *partition.root_payload;
    return state.container.read_record_data(partition.source->index, SfsId{1}, 64U * 1024U, cancellation);
}

Result<void> set_root_payload(TransactionState &state, MutablePartition &partition, std::vector<std::byte> payload,
                              const CancellationToken &cancellation) {
    const auto *root = record(*partition.source, SfsId{1});
    if (root == nullptr || root->extents.size() != 1U || !root->continuation_clusters.empty() ||
        payload.size() > static_cast<std::size_t>(root->extents[0].cluster_count) * 1024U) {
        return std::unexpected{transaction_error("partition root relocation is not enabled for this transaction")};
    }
    Result<std::vector<std::byte>> raw = partition.root_index ? Result<std::vector<std::byte>>{*partition.root_index}
                                                              : read_raw(*state.source, root->record_offset.value, 72);
    if (!raw)
        return std::unexpected{raw.error()};
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    const auto size = static_cast<std::uint32_t>(payload.size());
    ByteWriter writer{*raw};
    if (auto written = writer.write_be32(6, size); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(0x12, size); !written)
        return std::unexpected{written.error()};
    const auto count = static_cast<std::uint16_t>(payload.size() / 32U - 2U);
    if (auto written = writer.write_be16(0x46, count); !written)
        return std::unexpected{written.error()};
    partition.root_payload = std::move(payload);
    partition.root_index = std::move(*raw);
    return {};
}

Result<void> replace_record_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                    std::vector<std::byte> payload, const CancellationToken &cancellation) {
    if (partition.deleted.contains(id)) {
        return std::unexpected{transaction_error("cannot change a deleted SFS record")};
    }
    MutablePartition::InsertedRecord *target{};
    if (const auto found = partition.inserted.find(id); found != partition.inserted.end()) {
        target = &found->second;
    } else if (const auto changed = partition.changed.find(id); changed != partition.changed.end()) {
        target = &changed->second;
    } else {
        const auto *source = record(*partition.source, id);
        if (source == nullptr) {
            return std::unexpected{transaction_error("cannot change a missing SFS record")};
        }
        auto raw = read_raw(*state.source, source->record_offset.value, 72U);
        if (!raw)
            return std::unexpected{raw.error()};
        auto original = current_payload(state, partition, id, cancellation);
        if (!original)
            return std::unexpected{original.error()};
        auto [inserted, unused] = partition.changed.emplace(
            id, MutablePartition::InsertedRecord{id, std::move(*raw), std::move(*original), source->extents,
                                                 source->continuation_clusters, source->payload_kind});
        static_cast<void>(unused);
        target = &inserted->second;
    }

    std::uint64_t capacity{};
    for (const auto &extent : target->extents) {
        capacity += static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
    }
    if (payload.size() > capacity) {
        return std::unexpected{transaction_error("record payload growth exceeds its current extent capacity")};
    }
    if (target->capacity_expanded || target->extents.size() > 4U) {
        if (auto normalized = normalize_extent_byte_counts(target->extents, payload.size()); !normalized)
            return std::unexpected{normalized.error()};
        const ByteReader current_index{target->raw_index};
        const auto tail = current_index.be16(0x46U);
        if (!tail)
            return std::unexpected{tail.error()};
        detail::PreparedRecord prepared;
        prepared.kind =
            target->payload_kind == PayloadKind::directory ? detail::RecordKind::directory : detail::RecordKind::object;
        prepared.tail = *tail;
        auto encoded = detail::encode_sfs_index_record(
            prepared, target->extents, static_cast<std::uint32_t>(payload.size()), target->continuation_clusters);
        if (!encoded)
            return std::unexpected{encoded.error()};
        target->raw_index = std::move(*encoded);
        target->payload = std::move(payload);
        return {};
    }
    ByteWriter writer{target->raw_index};
    if (auto written = writer.write_be32(6U, static_cast<std::uint32_t>(payload.size())); !written)
        return std::unexpected{written.error()};
    if (target->extents.size() <= 4U) {
        std::uint32_t remaining = static_cast<std::uint32_t>(payload.size());
        for (std::size_t index = 0; index < target->extents.size(); ++index) {
            const auto capacity_for_extent = target->extents[index].cluster_count * 1024U;
            const auto byte_count = remaining == 0U ? capacity_for_extent : std::min(remaining, capacity_for_extent);
            remaining = remaining > byte_count ? remaining - byte_count : 0U;
            if (auto written = writer.write_be32(0x12U + index * 12U, byte_count); !written)
                return std::unexpected{written.error()};
        }
    }
    if (target->extents.size() == 1U) {
        if (auto written = writer.write_be32(0x12U, static_cast<std::uint32_t>(payload.size())); !written)
            return std::unexpected{written.error()};
    }
    if (id.value == 1U) {
        const auto entries = parse_directory(payload, id);
        if (!entries)
            return std::unexpected{entries.error()};
        if (auto written = writer.write_be16(0x46U, static_cast<std::uint16_t>(entries->size() - 2U)); !written)
            return std::unexpected{written.error()};
    }
    target->payload = std::move(payload);
    return {};
}

Result<SfsId> unique_directory_child(TransactionState &state, MutablePartition &partition, SfsId directory,
                                     std::string_view name, const CancellationToken &cancellation) {
    if (current_payload_kind(partition, directory) != PayloadKind::directory) {
        return std::unexpected{transaction_error("directory path resolves to a non-directory record")};
    }
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    std::vector<SfsId> matches;
    for (const auto &entry : *entries) {
        if (entry.name == name)
            matches.push_back(entry.id);
    }
    if (matches.size() != 1U) {
        return std::unexpected{transaction_error("directory requires exactly one entry named " + std::string{name})};
    }
    if (!record_exists(partition, matches.front())) {
        return std::unexpected{transaction_error("directory entry references a missing SFS record")};
    }
    return matches.front();
}

Result<SfsId> volume_category(TransactionState &state, MutablePartition &partition, std::string_view volume_name,
                              std::string_view category_name, const CancellationToken &cancellation) {
    auto volume = unique_directory_child(state, partition, SfsId{1}, volume_name, cancellation);
    if (!volume)
        return std::unexpected{volume.error()};
    return unique_directory_child(state, partition, *volume, category_name, cancellation);
}

Result<std::pair<SfsId, SfsId>> category_object(TransactionState &state, MutablePartition &partition,
                                                std::string_view volume_name, std::string_view category_name,
                                                std::string_view object_name, std::string_view expected_type,
                                                const CancellationToken &cancellation) {
    auto category = volume_category(state, partition, volume_name, category_name, cancellation);
    if (!category)
        return std::unexpected{category.error()};
    auto object = unique_directory_child(state, partition, *category, object_name, cancellation);
    if (!object)
        return std::unexpected{object.error()};
    auto payload = current_payload(state, partition, *object, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (payload->size() < 16U ||
        !std::equal(expected_type.begin(), expected_type.end(), payload->begin() + 12U, [](char left, std::byte right) {
            return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
        })) {
        return std::unexpected{transaction_error(std::string{object_name} + " does not resolve to one readable " +
                                                 std::string{expected_type} + " record")};
    }
    return std::pair{*category, *object};
}

Result<std::vector<CategoryObject>> category_objects(TransactionState &state, MutablePartition &partition,
                                                     std::string_view volume_name, std::string_view category_name,
                                                     ObjectType expected_type, const CancellationToken &cancellation) {
    auto directory = volume_category(state, partition, volume_name, category_name, cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto payload = current_payload(state, partition, *directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};
    std::vector<CategoryObject> result;
    for (const auto &entry : *entries) {
        if (entry.name == "." || entry.name == "..")
            continue;
        auto object_payload = current_payload(state, partition, entry.id, cancellation);
        if (!object_payload)
            return std::unexpected{object_payload.error()};
        auto decoded = decode_object(*object_payload);
        if (!decoded || decoded->header.type != expected_type) {
            return std::unexpected{transaction_error("category contains an unresolved or incorrectly typed object")};
        }
        result.push_back(CategoryObject{entry.name, entry.id, std::move(*object_payload), std::move(*decoded)});
    }
    return result;
}

Result<void> replace_fixed_object_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                          std::vector<std::byte> payload, const CancellationToken &cancellation) {
    auto current = current_payload(state, partition, id, cancellation);
    if (!current)
        return std::unexpected{current.error()};
    if (payload.size() != current->size()) {
        return std::unexpected{transaction_error("fixed-size object metadata update changed payload size")};
    }
    return replace_record_payload(state, partition, id, std::move(payload), cancellation);
}

Result<bool> sbnk_program_bit(std::span<const std::byte> payload, std::uint8_t program) {
    const auto offset = 0xc0U + static_cast<std::size_t>((program - 1U) / 32U) * 4U;
    if (payload.size() < offset + 4U) {
        return std::unexpected{transaction_error("SBNK payload is too short for its Program-link bitmap")};
    }
    const auto word = (std::to_integer<std::uint32_t>(payload[offset]) << 24U) |
                      (std::to_integer<std::uint32_t>(payload[offset + 1U]) << 16U) |
                      (std::to_integer<std::uint32_t>(payload[offset + 2U]) << 8U) |
                      std::to_integer<std::uint32_t>(payload[offset + 3U]);
    return (word & (std::uint32_t{1} << ((program - 1U) % 32U))) != 0U;
}

Result<void> set_sbnk_program_bit(TransactionState &state, MutablePartition &partition, SfsId id, std::uint8_t program,
                                  bool enabled, const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto current = sbnk_program_bit(*payload, program);
    if (!current)
        return std::unexpected{current.error()};
    const auto offset = 0xc0U + static_cast<std::size_t>((program - 1U) / 32U) * 4U;
    auto word = (std::to_integer<std::uint32_t>((*payload)[offset]) << 24U) |
                (std::to_integer<std::uint32_t>((*payload)[offset + 1U]) << 16U) |
                (std::to_integer<std::uint32_t>((*payload)[offset + 2U]) << 8U) |
                std::to_integer<std::uint32_t>((*payload)[offset + 3U]);
    const auto mask = std::uint32_t{1} << ((program - 1U) % 32U);
    word = enabled ? word | mask : word & ~mask;
    ByteWriter writer{*payload};
    if (auto written = writer.write_be32(offset, word); !written)
        return std::unexpected{written.error()};
    return replace_fixed_object_payload(state, partition, id, std::move(*payload), cancellation);
}

Result<void> set_sbnk_sample_bank_flag(TransactionState &state, MutablePartition &partition, SfsId id, bool enabled,
                                       const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (payload->size() <= 0xd0U) {
        return std::unexpected{
            transaction_error("Sample (SBNK) payload is too short for its Sample Bank membership flag")};
    }
    auto value = std::to_integer<std::uint8_t>((*payload)[0xd0U]);
    value = enabled ? static_cast<std::uint8_t>(value | 1U) : static_cast<std::uint8_t>(value & 0xfeU);
    (*payload)[0xd0U] = static_cast<std::byte>(value);
    return replace_fixed_object_payload(state, partition, id, std::move(*payload), cancellation);
}

Result<void> remove_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view name, const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    const auto found = std::ranges::find_if(
        *entries, [&](const ParsedDirectoryEntry &entry) { return entry.id == child && entry.name == name; });
    if (found == entries->end()) {
        return std::unexpected{transaction_error("directory entry is absent from transaction state")};
    }
    payload->erase(payload->begin() + static_cast<std::ptrdiff_t>(found->offset),
                   payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 32U));
    return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<PartitionIndex> resolve_partition(const TransactionState &state, const PartitionSelector &selector) {
    if (const auto *direct = std::get_if<PartitionIndex>(&selector))
        return *direct;
    const auto &reference = std::get<OperationReference>(selector);
    const auto found = std::ranges::find(state.reports, reference.operation_id, &OperationReport::id);
    if (found == state.reports.end()) {
        return std::unexpected{transaction_error("operation reference has no earlier result")};
    }
    return found->partition;
}

Result<std::set<SfsId>> volume_closure(const Partition &partition, const DirectoryEntry &volume) {
    std::set<SfsId> result;
    std::vector<SfsId> queue{SfsId{volume.link_id.value}};
    while (!queue.empty()) {
        const auto id = queue.front();
        queue.erase(queue.begin());
        if (result.contains(id))
            continue;
        const auto *item = record(partition, id);
        if (item == nullptr)
            return std::unexpected{transaction_error("volume closure references a missing SFS record")};
        result.insert(id);
        if (item->payload_kind != PayloadKind::directory)
            continue;
        for (const auto &child : item->directory_entries) {
            if (child.name != "." && child.name != "..")
                queue.push_back(SfsId{child.link_id.value});
        }
    }
    for (const auto &item : partition.records) {
        if (result.contains(item.sfs_id) || item.sfs_id.value == 1U || item.payload_kind != PayloadKind::directory)
            continue;
        for (const auto &child : item.directory_entries) {
            if (child.name != "." && child.name != ".." && result.contains(SfsId{child.link_id.value})) {
                return std::unexpected{transaction_error("a directory outside the volume references its closure")};
            }
        }
    }
    return result;
}

} // namespace axk::alteration_internal
