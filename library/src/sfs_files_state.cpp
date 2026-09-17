#include "sfs_files_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/bytes.hpp"
#include "sfs_cluster_allocation.hpp"

namespace axk::sfs_files {
Error error(std::string message, ErrorCode code) {
    return make_error(code,
                      code == ErrorCode::unsupported_profile ? ErrorCategory::unsupported : ErrorCategory::transaction,
                      std::move(message));
}

Result<void> validate(const Container &container, PartitionIndex index) {
    const auto found = std::ranges::find(container.partitions(), index, &Partition::index);
    if (found == container.partitions().end())
        return std::unexpected{error("filesystem partition does not exist")};
    const auto &partition = *found;
    const auto cluster_bytes =
        static_cast<std::uint64_t>(partition.sectors_per_cluster) * container.superblock().sector_size_bytes;
    if (cluster_bytes < 512U || cluster_bytes > 65536U)
        return std::unexpected{error("filesystem cluster geometry is outside the supported bounds")};
    if (!container.backup_superblock_matches() || !partition.backup_header_matches)
        return std::unexpected{error("filesystem backup headers disagree; raw editing is disabled")};
    if (!partition.diagnostics.empty())
        return std::unexpected{partition.diagnostics.front()};
    if (!allocation_is_safe_for_mutation(partition.allocation))
        return std::unexpected{
            error(std::format("filesystem allocation is inconsistent (unclaimed {}, unallocated {}, conflicts {}); raw "
                              "editing is disabled",
                              partition.allocation.bitmap_copy1.marked_used_without_index_extent_count,
                              partition.allocation.bitmap_copy1.index_extent_marked_free_count,
                              partition.allocation.conflicting_cluster_count))};
    const auto root = locate_partition_root_record(partition);
    if (!root)
        return std::unexpected{root.error()};
    std::map<std::uint32_t, std::uint32_t> references;
    for (const auto &record : partition.records) {
        if ((record.attributes & 0x80000000U) == 0U)
            return std::unexpected{error("payload record is not marked live")};
        if ((record.attributes & 0x01ffffffU) == 0x00646972U && record.payload_kind != PayloadKind::directory)
            return std::unexpected{error("filesystem directory cannot be decoded safely")};
        if (record.payload_kind != PayloadKind::directory)
            continue;
        if (!record.directory_id || record.directory_id->value != record.sfs_id.value)
            return std::unexpected{error("directory identity differs from its record")};
        for (const auto &entry : record.directory_entries)
            if (entry.target_link_id)
                ++references[entry.target_link_id->value];
    }
    for (const auto &record : partition.records) {
        if (record.sfs_id.value == 0U || record.sfs_id.value == 2U)
            continue;
        if (references[record.sfs_id.value] != record.link_count)
            return std::unexpected{error("filesystem link count does not match directory references")};
    }
    return {};
}

std::uint64_t State::cluster_offset(std::uint32_t cluster) const {
    return static_cast<std::uint64_t>(partition.start_sector) * sector_bytes +
           static_cast<std::uint64_t>(cluster) * cluster_bytes;
}

Result<State> open(std::shared_ptr<const RandomAccessReader> source, PartitionIndex index,
                   const CancellationToken &cancellation) {
    OpenOptions options;
    options.cancellation = cancellation;
    auto container = open_image(source, {}, options);
    if (!container)
        return std::unexpected{container.error()};
    if (auto checked = validate(*container, index); !checked)
        return std::unexpected{checked.error()};
    State state;
    state.source = std::move(source);
    state.partition = *std::ranges::find(container->partitions(), index, &Partition::index);
    state.sector_bytes = container->superblock().sector_size_bytes;
    const auto cluster_bytes = static_cast<std::uint64_t>(state.partition.sectors_per_cluster) * state.sector_bytes;
    state.cluster_bytes = static_cast<std::uint32_t>(cluster_bytes);
    state.root = *locate_partition_root_record(state.partition);
    state.cancellation = cancellation;
    state.bitmap.resize((state.partition.cluster_count + 7ULL) / 8U);
    if (auto read =
            state.source->read_exact_at(state.cluster_offset(state.partition.bitmap_copy1_cluster), state.bitmap);
        !read)
        return std::unexpected{read.error()};
    std::vector<std::byte> block(state.cluster_bytes);
    for (std::uint32_t cluster = 0; cluster < state.partition.directory_index_span_clusters; ++cluster) {
        if (auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        if (auto read = state.source->read_exact_at(
                state.cluster_offset(state.partition.directory_index_cluster + cluster), block);
            !read)
            return std::unexpected{read.error()};
        for (std::uint32_t slot = 0; slot < state.cluster_bytes / 72U; ++slot)
            if ((ByteReader{block}.be32(slot * 72U + 0x42U).value() & 0x80000000U) != 0U)
                state.occupied_slots.insert(cluster * (state.cluster_bytes / 72U) + slot);
    }
    for (const auto &info : state.partition.records) {
        Record record;
        record.info = info;
        if (auto read = state.source->read_exact_at(info.record_offset.value, record.raw); !read)
            return std::unexpected{read.error()};
        state.records.emplace(info.sfs_id.value, std::move(record));
    }
    state.protected_records = {0U, 1U, 2U, state.root.value};
    std::vector<std::uint32_t> support;
    for (const auto &entry : state.records.at(state.root.value).info.directory_entries)
        if (entry.target_link_id && is_partition_support_root_entry(entry.name))
            support.push_back(entry.target_link_id->value);
    while (!support.empty()) {
        const auto id = support.back();
        support.pop_back();
        if (!state.protected_records.insert(id).second)
            continue;
        const auto record = state.records.find(id);
        if (record == state.records.end())
            continue;
        for (const auto &entry : record->second.info.directory_entries)
            if (entry.target_link_id && entry.name != "." && entry.name != "..")
                support.push_back(entry.target_link_id->value);
    }
    return state;
}

Result<void> State::load_directory(Record &record) {
    if (!record.directory.empty())
        return {};
    if (record.info.payload_kind != PayloadKind::directory || record.info.data_size > OpenOptions{}.max_directory_bytes)
        return std::unexpected{error("directory cannot be read within the traversal bounds")};
    record.directory.resize(record.info.data_size);
    std::size_t copied{};
    for (const auto &extent : record.info.extents) {
        if (auto checked = cancellation.check(); !checked)
            return checked;
        const auto size = static_cast<std::size_t>(extent.byte_count);
        if (size > record.directory.size() - copied)
            return std::unexpected{error("directory extent exceeds its logical size")};
        if (auto read = source->read_exact_at(cluster_offset(extent.cluster_offset),
                                              std::span{record.directory}.subspan(copied, size));
            !read)
            return read;
        copied += size;
    }
    const auto parsed_bytes = record.info.directory_entries.size() * 32U;
    if (copied != record.directory.size() || copied % 32U != 0U || parsed_bytes > copied ||
        !std::ranges::all_of(std::span{record.directory}.subspan(parsed_bytes),
                             [](std::byte byte) { return byte == std::byte{}; }))
        return std::unexpected{error("directory contains uninterpreted trailing data")};
    return {};
}

void bytes_patch(State &state, std::uint64_t offset, std::vector<std::byte> bytes) {
    const auto size = bytes.size();
    state.patches.push_back({offset, std::make_shared<MemoryReader>(std::move(bytes)), 0U, size});
}

namespace {
bool used(std::span<const std::byte> bitmap, std::uint32_t cluster) {
    return (std::to_integer<unsigned>(bitmap[cluster / 8U]) & (0x80U >> (cluster % 8U))) != 0U;
}
void mark(std::span<std::byte> bitmap, std::uint32_t cluster, bool value) {
    const auto mask = static_cast<std::byte>(0x80U >> (cluster % 8U));
    if (value)
        bitmap[cluster / 8U] |= mask;
    else
        bitmap[cluster / 8U] &= ~mask;
}
} // namespace

Result<std::vector<Extent>> State::allocate(std::uint32_t bytes, std::uint32_t attributes) {
    std::vector<Extent> result;
    if (bytes == 0U)
        return result;
    const bool large = (attributes & 0x20000000U) != 0U;
    const auto unit = large ? partition.large_allocation_unit_clusters : 1U;
    if (unit == 0U)
        return std::unexpected{error("large-unit allocation requires a nonzero partition allocation unit")};
    const auto required = (static_cast<std::uint64_t>(bytes) + cluster_bytes - 1U) / cluster_bytes;
    const auto wanted = large ? ((required + unit - 1U) / unit) * unit : std::max<std::uint64_t>(2U, required);
    if (wanted > std::numeric_limits<std::uint16_t>::max())
        return std::unexpected{error("file exceeds the SFS record cluster-count field")};
    const auto first_payload = partition.directory_index_cluster + partition.directory_index_span_clusters;
    // Reserved metadata is excluded independently of the stored allocation bits.
    const auto selected = detail::select_sfs_payload_clusters(
        first_payload, partition.cluster_count, static_cast<std::uint32_t>(wanted),
        [&](std::uint32_t cluster) { return used(bitmap, cluster); }, unit);
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (!selected)
        return std::unexpected{error("filesystem has insufficient free clusters for the required allocation unit")};
    if (!large && bytes <= cluster_bytes && selected->at(1U) != selected->front() + 1U)
        return std::unexpected{error("minimum file allocation requires a contiguous extent")};
    auto remaining_bytes = bytes;
    for (std::size_t i = 0; i < selected->size();) {
        const auto start = selected->at(i++);
        std::uint32_t count = 1U;
        while (i < selected->size() && selected->at(i) == start + count) {
            ++i;
            ++count;
        }
        const auto extent_bytes = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining_bytes, static_cast<std::uint64_t>(count) * cluster_bytes));
        result.push_back({start, count, extent_bytes});
        remaining_bytes -= extent_bytes;
    }
    for (const auto &extent : result)
        if (extent.cluster_offset % unit != 0U || extent.cluster_count % unit != 0U)
            return std::unexpected{error("payload extent violates the SFS allocation unit")};
    for (const auto cluster : *selected)
        mark(bitmap, cluster, true);
    return result;
}

Result<void> State::release(Record &record) {
    for (const auto &extent : record.info.extents)
        for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
             ++cluster)
            mark(bitmap, cluster, false);
    for (const auto cluster : record.info.continuation_clusters)
        mark(bitmap, cluster, false);
    record.info.extents.clear();
    record.info.continuation_clusters.clear();
    record.info.cluster_count = 0;
    record.info.extent_count = 0;
    record.info.data_size = 0;
    record.changed = true;
    record.payload_changed = true;
    return {};
}

Result<void> State::change_links(Record &record, int delta) {
    const auto count = static_cast<int>(record.info.link_count) + delta;
    if (count < 0 || count > std::numeric_limits<std::uint16_t>::max())
        return std::unexpected{error("filesystem link-count overflow or underflow")};
    record.info.link_count = static_cast<std::uint16_t>(count);
    record.changed = true;
    return ByteWriter{record.raw}.write_be32(0x3eU, 0xffffffffU);
}

Result<Record *> State::create(bool directory) {
    const auto slots = static_cast<std::uint64_t>(partition.directory_index_span_clusters) * (cluster_bytes / 72U);
    for (std::uint32_t id = 3; id < slots; ++id) {
        if (occupied_slots.contains(id))
            continue;
        occupied_slots.insert(id);
        Record record;
        record.info.sfs_id = SfsId{id};
        record.info.record_offset =
            ByteOffset{cluster_offset(partition.directory_index_cluster + id / (cluster_bytes / 72U)) +
                       (id % (cluster_bytes / 72U)) * 72U};
        record.info.attributes = directory ? 0x94646972U : 0x9e000000U;
        record.info.link_count = directory ? 2U : 1U;
        record.info.payload_kind = directory ? PayloadKind::directory : PayloadKind::unknown;
        record.changed = true;
        record.payload_changed = true;
        ByteWriter writer{record.raw};
        if (auto written = writer.write_be32(0x3aU, 0xffffffffU); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x3eU, 0xffffffffU); !written)
            return std::unexpected{written.error()};
        return &records.insert_or_assign(id, std::move(record)).first->second;
    }
    return std::unexpected{error("filesystem index has no free records")};
}

Result<void> State::replace_payload(Record &record, std::shared_ptr<const RandomAccessReader> contents) {
    if (!contents || contents->size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected{error("file input is absent or exceeds the SFS size field")};
    if (auto released = release(record); !released)
        return released;
    auto extents = allocate(static_cast<std::uint32_t>(contents->size()), record.info.attributes);
    if (!extents)
        return std::unexpected{extents.error()};
    record.info.extents = std::move(*extents);
    record.info.extent_count = static_cast<std::uint16_t>(record.info.extents.size());
    record.info.data_size = static_cast<std::uint32_t>(contents->size());
    std::uint32_t clusters{};
    std::uint64_t source_offset{};
    for (const auto &extent : record.info.extents) {
        patches.push_back({cluster_offset(extent.cluster_offset), contents, source_offset, extent.byte_count});
        source_offset += extent.byte_count;
        clusters += extent.cluster_count;
    }
    record.info.cluster_count = static_cast<std::uint16_t>(clusters);
    return ByteWriter{record.raw}.write_be32(0x3eU, 0xffffffffU);
}

Result<void> State::encode(Record &record) {
    if (!record.payload_changed) {
        ByteWriter writer{record.raw};
        if (auto written = writer.write_be32(0x42U, record.info.attributes); !written)
            return written;
        if (auto written = writer.write_be16(0x46U, record.info.link_count); !written)
            return written;
        bytes_patch(*this, record.info.record_offset.value, {record.raw.begin(), record.raw.end()});
        return {};
    }
    ByteWriter writer{record.raw};
    if (record.deleted) {
        record.info.attributes &= 0x7fffffffU;
        if (auto written = writer.write_be32(0x3aU, 0U); !written)
            return written;
        if (auto written = writer.write_be32(0x3eU, 0U); !written)
            return written;
    }
    if (auto written = writer.write_be16(0U, record.info.extent_count); !written)
        return written;
    if (auto written = writer.write_be16(4U, record.info.cluster_count); !written)
        return written;
    if (auto written = writer.write_be32(6U, record.info.data_size); !written)
        return written;
    if (auto written = writer.write_be32(0x42U, record.info.attributes); !written)
        return written;
    if (auto written = writer.write_be16(0x46U, record.info.link_count); !written)
        return written;
    const auto per_block = (cluster_bytes - 12U) / 12U;
    if (record.info.extents.size() > 4U) {
        const auto block_count = (record.info.extents.size() + per_block - 1U) / per_block;
        for (std::size_t block = 0; block < block_count; ++block) {
            const auto found = [&]() -> std::uint32_t {
                for (std::uint32_t cluster =
                         partition.directory_index_cluster + partition.directory_index_span_clusters;
                     cluster < partition.cluster_count; ++cluster)
                    if (!used(bitmap, cluster)) {
                        mark(bitmap, cluster, true);
                        return cluster;
                    }
                return 0;
            }();
            if (found == 0)
                return std::unexpected{error("filesystem has no space for extent metadata")};
            record.info.continuation_clusters.push_back(found);
        }
    }
    const auto triplet = [](ByteWriter &target, std::size_t offset, const Extent &extent) -> Result<void> {
        if (auto written = target.write_be32(offset, extent.cluster_offset); !written)
            return written;
        if (auto written = target.write_be32(offset + 4U, extent.cluster_count); !written)
            return written;
        return target.write_be32(offset + 8U, extent.byte_count);
    };
    if (record.info.extents.size() <= 4U) {
        for (std::size_t i = 0; i < record.info.extents.size(); ++i)
            if (auto written = triplet(writer, 0x0aU + i * 12U, record.info.extents[i]); !written)
                return written;
    } else {
        if (auto written = writer.write_be32(0x0aU, record.info.continuation_clusters.front()); !written)
            return written;
        for (std::size_t i = 0; i < record.info.continuation_clusters.size(); ++i) {
            std::vector<std::byte> block(cluster_bytes);
            ByteWriter target{block};
            const auto begin = i * per_block;
            const auto count = std::min<std::size_t>(per_block, record.info.extents.size() - begin);
            if (auto written = target.write_be32(0U, static_cast<std::uint32_t>(count)); !written)
                return written;
            if (auto written = target.write_be32(8U, i + 1U < record.info.continuation_clusters.size()
                                                         ? record.info.continuation_clusters[i + 1U]
                                                         : 0U);
                !written)
                return written;
            for (std::size_t j = 0; j < count; ++j)
                if (auto written = triplet(target, 12U + j * 12U, record.info.extents[begin + j]); !written)
                    return written;
            bytes_patch(*this, cluster_offset(record.info.continuation_clusters[i]), std::move(block));
        }
    }
    bytes_patch(*this, record.info.record_offset.value, {record.raw.begin(), record.raw.end()});
    return {};
}

Result<void> State::finish() {
    for (auto &[id, record] : records) {
        static_cast<void>(id);
        if (auto checked = cancellation.check(); !checked)
            return checked;
        // Name-only changes preserve directory allocation, index metadata and slack.
        if (record.directory_renamed && !record.directory_changed && !record.deleted) {
            auto contents = std::make_shared<MemoryReader>(record.directory);
            std::uint64_t consumed{};
            for (const auto &extent : record.info.extents) {
                const auto count = std::min<std::uint64_t>(extent.byte_count, contents->size() - consumed);
                if (count != 0U)
                    patches.push_back({cluster_offset(extent.cluster_offset), contents, consumed, count});
                consumed += count;
            }
            if (consumed != contents->size())
                return std::unexpected(error("renamed directory exceeds its existing extents"));
        }
        if (!record.changed)
            continue;
        if (record.directory_changed && !record.deleted)
            if (auto replaced = replace_payload(record, std::make_shared<MemoryReader>(record.directory)); !replaced)
                return replaced;
        if (auto encoded = encode(record); !encoded)
            return encoded;
    }
    bytes_patch(*this, cluster_offset(partition.bitmap_copy1_cluster), bitmap);
    bytes_patch(*this, cluster_offset(partition.bitmap_copy2_cluster), bitmap);
    return {};
}
} // namespace axk::sfs_files

axk::Result<void> axk::detail::inspect_sfs_file_edit_support(const Container &container, PartitionIndex partition) {
    return sfs_files::validate(container, partition);
}
