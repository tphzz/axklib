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
#include "sfs_cluster_allocation.hpp"

namespace axk::alteration_internal {

Result<std::optional<PartitionObjectSet>> plan_volume_deletion_batch(const TransactionState &state,
                                                                     const AlterationManifest &manifest,
                                                                     const CancellationToken &cancellation) {
    const auto direct_deletions = std::ranges::all_of(manifest.operations, [](const AlterationOperation &operation) {
        const auto *deletion = std::get_if<DeleteVolumeOperation>(&operation.data);
        return deletion != nullptr && std::holds_alternative<PartitionIndex>(deletion->partition);
    });
    if (!direct_deletions)
        return std::optional<PartitionObjectSet>{};

    std::set<std::pair<std::uint8_t, std::string>> targets;
    PartitionObjectSet closure_union;
    for (const auto &typed_operation : manifest.operations) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto &operation = std::get<DeleteVolumeOperation>(typed_operation.data);
        const auto partition_index = std::get<PartitionIndex>(operation.partition);
        if (!targets.emplace(partition_index.value, operation.volume_name).second) {
            return std::unexpected{transaction_error("volume deletion targets must be unique")};
        }
        const auto partition_state = state.partitions.find(partition_index.value);
        if (partition_state == state.partitions.end()) {
            return std::unexpected{transaction_error("partition index does not exist")};
        }
        const auto &partition = *partition_state->second.source;
        const auto *root = record(partition, SfsId{1U});
        if (root == nullptr || root->payload_kind != PayloadKind::directory) {
            return std::unexpected{transaction_error("partition root directory is unavailable")};
        }
        std::vector<const DirectoryEntry *> matches;
        for (const auto &entry : root->directory_entries) {
            if (entry.state == DirectoryEntryState::live && entry.name == operation.volume_name)
                matches.push_back(&entry);
        }
        if (matches.size() != 1U) {
            return std::unexpected{transaction_error("volume name is not unique in the selected partition")};
        }
        auto closure = volume_closure(partition, *matches.front());
        if (!closure)
            return std::unexpected{closure.error()};
        for (const auto id : *closure)
            closure_union.emplace(partition_index, id);
    }
    for (const auto &[partition, source, target] : state.known_edges) {
        if (closure_union.contains({partition, source}) != closure_union.contains({partition, target})) {
            return std::unexpected{transaction_error("a known object relationship crosses the volume deletion batch")};
        }
    }
    return std::optional<PartitionObjectSet>{std::move(closure_union)};
}

Result<OperationReport> delete_volume(TransactionState &state, OperationContext context,
                                      const DeleteVolumeOperation &operation, const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    if (is_partition_support_root_entry(operation.volume_name))
        return std::unexpected{transaction_error("PRF3 is a reserved partition support directory")};
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto partition_state = state.partitions.find(partition_index->value);
    if (partition_state == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &mutable_partition = partition_state->second;
    const auto &partition = *mutable_partition.source;
    const auto *root = record(partition, SfsId{1});
    if (root == nullptr || root->payload_kind != PayloadKind::directory || root->extents.size() != 1U ||
        !root->continuation_clusters.empty()) {
        return std::unexpected{transaction_error("partition root must use one readable direct extent")};
    }
    std::vector<const DirectoryEntry *> matches;
    for (const auto &entry : root->directory_entries) {
        if (entry.state == DirectoryEntryState::live && entry.name == operation.volume_name)
            matches.push_back(&entry);
    }
    if (matches.size() != 1U)
        return std::unexpected{transaction_error("volume name is not unique in the selected partition")};
    auto closure = volume_closure(partition, *matches.front());
    if (!closure)
        return std::unexpected{closure.error()};

    if (state.approved_volume_deletion_batch) {
        const auto outside_batch = std::ranges::any_of(*closure, [&](const SfsId id) {
            return !state.approved_volume_deletion_batch->contains({partition.index, id});
        });
        if (outside_batch) {
            return std::unexpected{transaction_error("volume deletion is outside the approved batch")};
        }
    } else {
        for (const auto &[edge_partition, source, target] : state.known_edges) {
            if (edge_partition == partition.index && closure->contains(source) != closure->contains(target)) {
                return std::unexpected{transaction_error("a known object relationship crosses the volume closure")};
            }
        }
    }
    auto payload = current_root_payload(state, mutable_partition, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto current_entries = parse_directory(*payload, SfsId{1});
    if (!current_entries)
        return std::unexpected{current_entries.error()};
    const auto current = std::ranges::find_if(*current_entries, [&](const ParsedDirectoryEntry &entry) {
        return entry.state == DirectoryEntryState::live && entry.name == operation.volume_name &&
               entry.target_sfs_id == SfsId{matches.front()->target_link_id->value};
    });
    if (current == current_entries->end()) {
        return std::unexpected{transaction_error("volume directory entry is absent from transaction state")};
    }
    payload->erase(payload->begin() + static_cast<std::ptrdiff_t>(current->offset),
                   payload->begin() + static_cast<std::ptrdiff_t>(current->offset + 32U));
    if (auto replaced = set_root_payload(state, mutable_partition, std::move(*payload), cancellation); !replaced)
        return std::unexpected{replaced.error()};
    std::uint64_t freed{};
    for (const auto id : *closure) {
        const auto *item = record(partition, id);
        for (const auto &extent : item->extents) {
            for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
                 ++cluster) {
                set_bitmap(mutable_partition.bitmap, cluster, false);
                ++freed;
            }
        }
        for (const auto cluster : item->continuation_clusters) {
            set_bitmap(mutable_partition.bitmap, cluster, false);
            ++freed;
        }
        mutable_partition.deleted.insert(id);
    }
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = partition.index;
    report.volume_name = operation.volume_name;
    report.freed_clusters = freed;
    report.removed_sfs_ids.assign(closure->begin(), closure->end());
    return report;
}

std::vector<SfsId> free_ids(const MutablePartition &partition, std::size_t count) {
    const auto capacity = (static_cast<std::uint64_t>(partition.source->directory_index_span_clusters) *
                           partition.source->sectors_per_cluster * 512U / 1024U) *
                          14U;
    std::vector<SfsId> result;
    for (std::uint32_t value = 3; value < capacity && result.size() < count; ++value) {
        const SfsId id{value};
        if (!partition.inserted.contains(id) && !partition.changed.contains(id) &&
            (partition.deleted.contains(id) || record(*partition.source, id) == nullptr)) {
            result.push_back(id);
        }
    }
    return result;
}

Result<std::vector<Extent>> allocate_extents(MutablePartition &partition, std::uint32_t count) {
    const auto first = partition.source->directory_index_cluster + partition.source->directory_index_span_clusters;
    auto selected = detail::select_sfs_payload_clusters(
        first, partition.source->cluster_count, count,
        [&](const std::uint32_t cluster) { return bitmap_used(partition.bitmap, cluster); });
    if (!selected)
        return std::unexpected{transaction_error("partition has insufficient free clusters")};
    std::vector<Extent> result;
    for (const auto cluster : *selected) {
        set_bitmap(partition.bitmap, cluster, true);
        if (!result.empty() && result.back().cluster_offset + result.back().cluster_count == cluster) {
            ++result.back().cluster_count;
            result.back().byte_count += 1024U;
        } else {
            result.push_back({cluster, 1, 1024});
        }
    }
    return result;
}

Result<std::vector<std::uint32_t>> allocate_list_clusters(MutablePartition &partition, std::size_t count) {
    std::vector<std::uint32_t> result;
    const auto first = partition.source->directory_index_cluster + partition.source->directory_index_span_clusters;
    for (std::uint32_t cluster = first; cluster < partition.source->cluster_count && result.size() < count; ++cluster) {
        if (!bitmap_used(partition.bitmap, cluster))
            result.push_back(cluster);
    }
    if (result.size() != count) {
        return std::unexpected{transaction_error("partition has insufficient continuation-list clusters")};
    }
    for (const auto cluster : result)
        set_bitmap(partition.bitmap, cluster, true);
    return result;
}

std::vector<Extent> merge_extents(std::span<const Extent> existing, std::span<const Extent> added) {
    std::vector<Extent> result;
    result.reserve(existing.size() + added.size());
    const auto append = [&result](const Extent &extent) {
        if (!result.empty() && result.back().cluster_offset + result.back().cluster_count == extent.cluster_offset) {
            result.back().cluster_count += extent.cluster_count;
            result.back().byte_count += extent.byte_count;
        } else {
            result.push_back(extent);
        }
    };
    std::ranges::for_each(existing, append);
    std::ranges::for_each(added, append);
    return result;
}

Result<void> normalize_extent_byte_counts(std::span<Extent> extents, std::size_t payload_size) {
    if (payload_size > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected{transaction_error("record payload exceeds its encoded size")};
    const auto byte_counts = detail::plan_extent_byte_counts(extents, static_cast<std::uint32_t>(payload_size));
    if (!byte_counts)
        return std::unexpected{transaction_error(byte_counts.error().message)};
    for (std::size_t index = 0; index < extents.size(); ++index)
        extents[index].byte_count = (*byte_counts)[index];
    return {};
}

Result<std::pair<std::uint64_t, std::uint64_t>> grow_directory_capacity(TransactionState &state,
                                                                        MutablePartition &partition, SfsId id,
                                                                        std::uint64_t required_size,
                                                                        const CancellationToken &cancellation) {
    if (current_payload_kind(partition, id) != PayloadKind::directory)
        return std::unexpected{transaction_error("only SFS directory records may grow")};
    MutablePartition::InsertedRecord *target{};
    if (const auto found = partition.inserted.find(id); found != partition.inserted.end()) {
        target = &found->second;
    } else if (const auto changed = partition.changed.find(id); changed != partition.changed.end()) {
        target = &changed->second;
    } else {
        const auto *source = record(*partition.source, id);
        if (source == nullptr)
            return std::unexpected{transaction_error("cannot grow a missing SFS directory")};
        auto raw = read_raw(*state.source, source->record_offset.value, 72U);
        if (!raw)
            return std::unexpected{raw.error()};
        auto payload = current_payload(state, partition, id, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        auto [inserted, unused] = partition.changed.emplace(
            id, MutablePartition::InsertedRecord{id, std::move(*raw), std::move(*payload), source->extents,
                                                 source->continuation_clusters, source->payload_kind});
        static_cast<void>(unused);
        target = &inserted->second;
    }
    std::uint64_t capacity{};
    for (const auto &extent : target->extents)
        capacity += static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
    if (required_size <= capacity)
        return std::pair<std::uint64_t, std::uint64_t>{};
    const auto required_clusters = (required_size + 1023U) / 1024U;
    const auto current_clusters = capacity / 1024U;
    if (required_clusters > std::numeric_limits<std::uint32_t>::max() ||
        required_clusters - current_clusters > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{transaction_error("SFS directory growth exceeds the supported cluster count")};
    }
    const auto additional_clusters = static_cast<std::uint32_t>(required_clusters - current_clusters);
    auto added = allocate_extents(partition, additional_clusters);
    if (!added)
        return std::unexpected{added.error()};
    auto extents = merge_extents(target->extents, *added);
    constexpr std::size_t extents_per_list_cluster = (1024U - 12U) / 12U;
    const auto required_lists =
        extents.size() <= 4U ? 0U : (extents.size() + extents_per_list_cluster - 1U) / extents_per_list_cluster;
    if (required_lists < target->continuation_clusters.size())
        return std::unexpected{transaction_error("SFS directory growth cannot discard continuation lists")};
    const auto additional_lists = required_lists - target->continuation_clusters.size();
    if (additional_lists != 0U) {
        auto lists = allocate_list_clusters(partition, additional_lists);
        if (!lists)
            return std::unexpected{lists.error()};
        target->continuation_clusters.insert(target->continuation_clusters.end(), lists->begin(), lists->end());
    }
    target->extents = std::move(extents);
    target->capacity_expanded = true;
    return std::pair{static_cast<std::uint64_t>(additional_clusters), static_cast<std::uint64_t>(additional_lists)};
}

Result<std::pair<SfsId, std::uint64_t>> allocate_record(MutablePartition &partition, std::vector<std::byte> payload,
                                                        PayloadKind payload_kind, std::optional<SfsId> requested_id,
                                                        std::uint16_t directory_tail) {
    const auto ids = requested_id ? std::vector{*requested_id} : free_ids(partition, 1U);
    if (ids.empty() ||
        (requested_id && (record_exists(partition, *requested_id) || partition.inserted.contains(*requested_id)))) {
        return std::unexpected{transaction_error("partition has no free SFS record")};
    }
    const auto clusters = std::max<std::uint32_t>(2U, static_cast<std::uint32_t>((payload.size() + 1023U) / 1024U));
    auto extents = allocate_extents(partition, clusters);
    if (!extents)
        return std::unexpected{extents.error()};
    if (auto normalized = normalize_extent_byte_counts(*extents, payload.size()); !normalized)
        return std::unexpected{normalized.error()};
    constexpr std::size_t extents_per_list_cluster = (1024U - 12U) / 12U;
    std::vector<std::uint32_t> list_clusters;
    if (extents->size() > 4U) {
        auto allocated_lists = allocate_list_clusters(partition, (extents->size() + extents_per_list_cluster - 1U) /
                                                                     extents_per_list_cluster);
        if (!allocated_lists)
            return std::unexpected{allocated_lists.error()};
        list_clusters = std::move(*allocated_lists);
    }
    detail::PreparedRecord prepared;
    prepared.kind = payload_kind == PayloadKind::directory ? detail::RecordKind::directory : detail::RecordKind::object;
    prepared.tail = directory_tail;
    auto raw =
        detail::encode_sfs_index_record(prepared, *extents, static_cast<std::uint32_t>(payload.size()), list_clusters);
    if (!raw)
        return std::unexpected{raw.error()};
    const auto id = ids.front();
    partition.deleted.erase(id);
    partition.inserted.emplace(id, MutablePartition::InsertedRecord{id, std::move(*raw), std::move(payload),
                                                                    std::move(*extents), std::move(list_clusters),
                                                                    payload_kind});
    return std::pair{id, clusters + partition.inserted.at(id).continuation_clusters.size()};
}

Result<void> append_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view name, const CancellationToken &cancellation) {
    if (name.empty() || name.size() > 16U ||
        !std::ranges::all_of(name, [](unsigned char value) { return value < 0x80U; })) {
        return std::unexpected{transaction_error("SFS object name must fit 16 ASCII bytes")};
    }
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const ParsedDirectoryEntry &entry) {
            return entry.state == DirectoryEntryState::live && entry.name == name;
        })) {
        return std::unexpected{transaction_error("directory already contains entry " + std::string{name})};
    }
    std::array<std::byte, 32> entry{};
    ByteWriter writer{entry};
    if (auto written = writer.write_be16(0U, 0x20U); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be16(2U, 17U); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(4U, child.value); !written)
        return std::unexpected{written.error()};
    std::fill(entry.begin() + 8U, entry.begin() + 24U, std::byte{' '});
    std::ranges::transform(name, entry.begin() + 8U, [](char value) { return static_cast<std::byte>(value); });
    const auto reusable = std::ranges::find_if(*entries, [&](const ParsedDirectoryEntry &candidate) {
        return candidate.state == DirectoryEntryState::deleted && candidate.name == name;
    });
    const auto fallback = std::ranges::find_if(*entries, [](const ParsedDirectoryEntry &candidate) {
        return candidate.state == DirectoryEntryState::deleted;
    });
    const auto selected = reusable != entries->end() ? reusable : fallback;
    if (selected != entries->end()) {
        std::ranges::copy(entry, payload->begin() + static_cast<std::ptrdiff_t>(selected->offset));
    } else {
        payload->insert(payload->end(), entry.begin(), entry.end());
    }
    if (auto grown = grow_directory_capacity(state, partition, directory, payload->size(), cancellation); !grown)
        return std::unexpected{grown.error()};
    return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<void> rename_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view old_name, std::string_view new_name,
                                    const CancellationToken &cancellation) {
    if (new_name.empty() || new_name.size() > 16U ||
        !std::ranges::all_of(new_name, [](unsigned char value) { return value < 0x80U; })) {
        return std::unexpected{transaction_error("SFS object name must fit 16 ASCII bytes")};
    }
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const auto &entry) {
            return entry.state == DirectoryEntryState::live && entry.name == new_name && entry.target_sfs_id != child;
        })) {
        return std::unexpected{transaction_error("rename destination already exists")};
    }
    const auto found = std::ranges::find_if(
        *entries, [&](const auto &entry) { return entry.target_sfs_id == child && entry.name == old_name; });
    if (found == entries->end()) {
        return std::unexpected{transaction_error("rename source directory entry is absent")};
    }
    ByteWriter writer{*payload};
    if (auto written = writer.write_be16(found->offset + 2U, 17U); !written)
        return std::unexpected{written.error()};
    std::fill(payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 8U),
              payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 24U), std::byte{' '});
    std::ranges::transform(new_name, payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 8U),
                           [](char value) { return static_cast<std::byte>(value); });
    (*payload)[found->offset + 24U] = std::byte{0};
    return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<void> rename_object_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                   std::string_view old_name, std::string_view new_name,
                                   const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (payload->size() < 0x42U) {
        return std::unexpected{transaction_error("object payload name is truncated")};
    }
    std::string actual;
    for (std::size_t offset = 0x32U; offset < 0x42U; ++offset) {
        const auto value = std::to_integer<unsigned char>((*payload)[offset]);
        if (value != 0U)
            actual.push_back(static_cast<char>(value));
    }
    while (!actual.empty() && actual.back() == ' ')
        actual.pop_back();
    if (actual != old_name) {
        return std::unexpected{transaction_error("object payload name disagrees with directory identity")};
    }
    std::fill(payload->begin() + 0x32, payload->begin() + 0x42, std::byte{' '});
    std::ranges::transform(new_name, payload->begin() + 0x32, [](char value) { return static_cast<std::byte>(value); });
    return replace_fixed_object_payload(state, partition, id, std::move(*payload), cancellation);
}

Result<std::uint64_t> release_record(MutablePartition &partition, SfsId id) {
    std::vector<Extent> extents;
    std::vector<std::uint32_t> continuation;
    if (const auto found = partition.inserted.find(id); found != partition.inserted.end()) {
        extents = found->second.extents;
        continuation = found->second.continuation_clusters;
        partition.inserted.erase(found);
    } else if (const auto changed = partition.changed.find(id); changed != partition.changed.end()) {
        extents = changed->second.extents;
        continuation = changed->second.continuation_clusters;
        partition.changed.erase(changed);
    } else if (const auto *source = record(*partition.source, id);
               source != nullptr && !partition.deleted.contains(id)) {
        extents = source->extents;
        continuation = source->continuation_clusters;
    } else {
        return std::unexpected{transaction_error("cannot release a missing SFS record")};
    }
    std::uint64_t released{};
    for (const auto &extent : extents) {
        for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
             ++cluster) {
            set_bitmap(partition.bitmap, cluster, false);
            ++released;
        }
    }
    for (const auto cluster : continuation) {
        set_bitmap(partition.bitmap, cluster, false);
        ++released;
    }
    partition.deleted.insert(id);
    return released;
}

Result<std::vector<std::byte>> remap_directory(std::vector<std::byte> payload,
                                               const std::map<std::uint32_t, SfsId> &ids) {
    const auto directory_id = payload.size() >= 8U ? (std::to_integer<std::uint32_t>(payload[4]) << 24U) |
                                                         (std::to_integer<std::uint32_t>(payload[5]) << 16U) |
                                                         (std::to_integer<std::uint32_t>(payload[6]) << 8U) |
                                                         std::to_integer<std::uint32_t>(payload[7])
                                                   : 0U;
    for (std::size_t offset = 0; offset + 32U <= payload.size(); offset += 32U) {
        const auto old = (std::to_integer<std::uint32_t>(payload[offset + 4U]) << 24U) |
                         (std::to_integer<std::uint32_t>(payload[offset + 5U]) << 16U) |
                         (std::to_integer<std::uint32_t>(payload[offset + 6U]) << 8U) |
                         std::to_integer<std::uint32_t>(payload[offset + 7U]);
        if (const auto found = ids.find(old); found != ids.end()) {
            ByteWriter writer{payload};
            if (auto written = writer.write_be32(offset + 4U, found->second.value); !written)
                return std::unexpected{written.error()};
        }
        if (old == 1U && payload[offset + 8U] == std::byte{'.'} && payload[offset + 9U] == std::byte{'.'}) {
            const auto mapped = ids.find(directory_id);
            payload[offset + 11U] = static_cast<std::byte>(mapped == ids.end() ? 0U : mapped->second.value & 0xffU);
        }
    }
    return payload;
}

Result<OperationReport> insert_volume(TransactionState &state, OperationContext context,
                                      const InsertVolumeOperation &operation, const CancellationToken &cancellation) {
    if (is_partition_support_root_entry(operation.volume.name))
        return std::unexpected{transaction_error("PRF3 is reserved for partition support files")};
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("insert-volume target is invalid")};
    auto &partition = found->second;
    auto root_payload = current_root_payload(state, partition, cancellation);
    if (!root_payload)
        return std::unexpected{root_payload.error()};
    auto root_entries = parse_directory(*root_payload, SfsId{1});
    if (!root_entries)
        return std::unexpected{root_entries.error()};
    if (std::ranges::any_of(*root_entries, [&](const ParsedDirectoryEntry &entry) {
            return entry.state == DirectoryEntryState::live && entry.name == operation.volume.name;
        })) {
        return std::unexpected{transaction_error("partition already contains the requested volume")};
    }
    HdsBuildManifest template_manifest{
        std::string{build_manifest_schema_version}, minimum_hds_size, {{"AXK ALTER", {operation.volume}}}};
    auto geometry = plan_hds_geometry(template_manifest);
    if (!geometry)
        return std::unexpected{geometry.error()};
    auto prepared = detail::prepare_partition_records(template_manifest.partitions[0], (*geometry)[0], 1, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    std::vector<detail::PreparedRecord> templates;
    std::ranges::copy_if(*prepared, std::back_inserter(templates), [](const auto &item) { return item.id >= 3U; });
    const auto ids = free_ids(partition, templates.size());
    if (ids.size() != templates.size())
        return std::unexpected{transaction_error("partition has insufficient free SFS records")};
    std::map<std::uint32_t, SfsId> id_map;
    for (std::size_t index = 0; index < templates.size(); ++index)
        id_map.emplace(templates[index].id, ids[index]);
    std::uint64_t allocated{};
    for (std::size_t index = 0; index < templates.size(); ++index) {
        Result<std::vector<std::byte>> payload = templates[index].kind == detail::RecordKind::directory
                                                     ? remap_directory(templates[index].payload, id_map)
                                                     : Result<std::vector<std::byte>>{templates[index].payload};
        if (!payload)
            return std::unexpected{payload.error()};
        auto stored = allocate_record(partition, std::move(*payload),
                                      templates[index].kind == detail::RecordKind::directory ? PayloadKind::directory
                                                                                             : PayloadKind::object,
                                      ids[index], templates[index].tail);
        if (!stored)
            return std::unexpected{stored.error()};
        allocated += stored->second;
    }
    std::array<std::byte, 32> entry{};
    ByteWriter writer{entry};
    if (auto written = writer.write_be16(0, 0x20); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be16(2, 17); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(4, id_map.at(3).value); !written)
        return std::unexpected{written.error()};
    const auto encoded = operation.volume.name;
    for (std::size_t index = 0; index < 16U; ++index)
        entry[8U + index] = index < encoded.size() ? static_cast<std::byte>(encoded[index]) : std::byte{' '};
    const auto matching_tombstone = std::ranges::find_if(*root_entries, [&](const ParsedDirectoryEntry &candidate) {
        return candidate.state == DirectoryEntryState::deleted && candidate.name == operation.volume.name;
    });
    const auto first_tombstone = std::ranges::find_if(*root_entries, [](const ParsedDirectoryEntry &candidate) {
        return candidate.state == DirectoryEntryState::deleted;
    });
    const auto selected = matching_tombstone != root_entries->end() ? matching_tombstone : first_tombstone;
    if (selected != root_entries->end()) {
        std::ranges::copy(entry, root_payload->begin() + static_cast<std::ptrdiff_t>(selected->offset));
    } else {
        root_payload->insert(root_payload->end(), entry.begin(), entry.end());
    }
    if (auto replaced = set_root_payload(state, partition, std::move(*root_payload), cancellation); !replaced)
        return std::unexpected{replaced.error()};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume.name;
    report.inserted_sfs_ids = ids;
    report.allocated_clusters = allocated;
    return report;
}

Result<OperationReport> rename_volume(TransactionState &state, OperationContext context,
                                      const RenameVolumeOperation &operation, const CancellationToken &cancellation) {
    if (operation.volume_name == operation.new_volume_name)
        return std::unexpected{transaction_error("new_volume_name must differ")};
    if (is_partition_support_root_entry(operation.volume_name) ||
        is_partition_support_root_entry(operation.new_volume_name)) {
        return std::unexpected{transaction_error("PRF3 is a reserved partition support directory")};
    }
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto payload = current_root_payload(state, partition, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, SfsId{1});
    if (!entries)
        return std::unexpected{entries.error()};
    const auto matches = std::ranges::count_if(*entries, [&](const ParsedDirectoryEntry &entry) {
        return entry.state == DirectoryEntryState::live && entry.name == operation.volume_name;
    });
    if (matches != 1U)
        return std::unexpected{transaction_error("volume name is not unique in the selected partition")};
    const auto source = std::ranges::find_if(*entries, [&](const ParsedDirectoryEntry &entry) {
        return entry.state == DirectoryEntryState::live && entry.name == operation.volume_name;
    });
    if (auto renamed = rename_directory_entry(state, partition, SfsId{1}, *source->target_sfs_id, operation.volume_name,
                                              operation.new_volume_name, cancellation);
        !renamed) {
        return std::unexpected{renamed.error()};
    }
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.new_volume_name;
    return report;
}

Result<OperationReport> rename_partition(TransactionState &state, OperationContext context,
                                         const RenamePartitionOperation &operation,
                                         const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (operation.partition_name == operation.new_partition_name)
        return std::unexpected{transaction_error("new_partition_name must differ")};
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    const auto &current_name = partition.renamed_name ? *partition.renamed_name : partition.source->name;
    if (current_name != operation.partition_name)
        return std::unexpected{transaction_error("partition name changed since the alteration was prepared")};
    partition.renamed_name = operation.new_partition_name;
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    return report;
}

} // namespace axk::alteration_internal
