#include "axklib/sfs.hpp"

#include "sfs_internal.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>

#include "axklib/bytes.hpp"
#include "axklib/sfs_allocation.hpp"

namespace axk::sfs_detail {

constexpr std::string_view magic{"YAMAHA_dev3"};
constexpr std::uint64_t partition_header_size = 1024;
constexpr std::uint64_t index_block_size = 1024;
constexpr std::uint64_t index_record_size = 72;
constexpr std::uint64_t records_per_index_block = 14;
constexpr std::uint16_t direct_extent_limit = 4;
constexpr std::uint64_t continuation_header_size = 12;

struct ParsedRecord {
    std::uint16_t extent_count{};
    std::uint16_t cluster_count{};
    std::uint32_t data_size{};
};

Result<std::vector<std::byte>> read_bytes(const RandomAccessReader &reader, std::uint64_t offset, std::size_t count,
                                          const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check) {
        return std::unexpected{check.error()};
    }
    std::vector<std::byte> result(count);
    if (const auto read = reader.read_exact_at(offset, result); !read) {
        return std::unexpected{read.error()};
    }
    if (const auto check = cancellation.check(); !check) {
        return std::unexpected{check.error()};
    }
    return result;
}

Result<std::uint64_t> cluster_offset(std::uint32_t partition_start_sector, std::uint32_t sector_size,
                                     std::uint32_t sectors_per_cluster, std::uint32_t cluster) {
    const auto relative_sectors = checked_multiply(cluster, sectors_per_cluster);
    if (!relative_sectors) {
        return std::unexpected{relative_sectors.error()};
    }
    const auto absolute_sector = checked_add(partition_start_sector, *relative_sectors);
    if (!absolute_sector) {
        return std::unexpected{absolute_sector.error()};
    }
    return checked_multiply(*absolute_sector, sector_size);
}

bool begins_with(std::span<const std::byte> bytes, std::string_view value) {
    if (bytes.size() < value.size()) {
        return false;
    }
    return std::equal(value.begin(), value.end(), bytes.begin(), [](char left, std::byte right) {
        return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
    });
}

Error partition_error(ErrorCode code, std::string message, PartitionIndex index, std::uint64_t offset) {
    ErrorContext context;
    context.partition_index = index.value;
    if (offset != 0) {
        context.raw_offset = offset;
    }
    return make_error(code, ErrorCategory::container, std::move(message), std::move(context));
}

Result<Superblock> parse_superblock(std::span<const std::byte> bytes) {
    if (bytes.size() != sfs_default_sector_size) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::container,
                                          "SFS superblock must contain exactly 512 bytes")};
    }
    if (!begins_with(bytes, magic)) {
        return std::unexpected{make_error(ErrorCode::container_unrecognized, ErrorCategory::container,
                                          "input does not begin with Yamaha SFS magic")};
    }
    const ByteReader reader{bytes};
    Superblock result;
    const auto sector_size = reader.be32(0x09c);
    const auto total_sectors = reader.be32(0x0a0);
    if (!sector_size || !total_sectors) {
        return std::unexpected{!sector_size ? sector_size.error() : total_sectors.error()};
    }
    result.sector_size_bytes = *sector_size;
    result.total_sector_count = *total_sectors;
    std::copy_n(bytes.begin() + 0x80, result.unresolved_formatter_words.size(),
                result.unresolved_formatter_words.begin());
    for (std::size_t index = 0; index < result.partition_entries.size(); ++index) {
        const auto relative = 0x0a8U + index * 8U;
        const auto start = reader.be32(relative);
        const auto count = reader.be32(relative + 4U);
        if (!start || !count) {
            return std::unexpected{!start ? start.error() : count.error()};
        }
        result.partition_entries[index] = {PartitionIndex{static_cast<std::uint8_t>(index)}, *start, *count};
    }
    return result;
}

std::uint32_t count_bitmap_bits(std::span<const std::byte> bitmap, std::uint32_t count) {
    std::uint32_t result{};
    for (std::uint32_t cluster = 0; cluster < count; ++cluster) {
        const auto byte = std::to_integer<std::uint8_t>(bitmap[cluster / 8U]);
        result += (byte & static_cast<std::uint8_t>(0x80U >> (cluster & 7U))) != 0 ? 1U : 0U;
    }
    return result;
}

bool bitmap_test(std::span<const std::byte> bitmap, std::uint32_t cluster) {
    const auto index = static_cast<std::size_t>(cluster / 8U);
    return index < bitmap.size() &&
           (std::to_integer<std::uint8_t>(bitmap[index]) & static_cast<std::uint8_t>(0x80U >> (cluster & 7U))) != 0;
}

bool bitmap_set(std::span<std::byte> bitmap, std::uint32_t cluster) {
    const auto index = static_cast<std::size_t>(cluster / 8U);
    if (index >= bitmap.size()) {
        return false;
    }
    bitmap[index] |= static_cast<std::byte>(0x80U >> (cluster & 7U));
    return true;
}

constexpr std::uint32_t owner_conflict_bit = 0x8000'0000U;
constexpr std::uint32_t reserved_owner = 1U;

std::uint32_t record_owner(SfsId record, AllocationClaimKind kind) {
    return 2U + record.value * 2U + (kind == AllocationClaimKind::continuation ? 1U : 0U);
}

AllocationClaim decode_owner(std::uint32_t encoded) {
    encoded &= ~owner_conflict_bit;
    if (encoded == reserved_owner)
        return {AllocationClaimKind::reserved, std::nullopt};
    const auto kind = (encoded & 1U) == 0U ? AllocationClaimKind::data : AllocationClaimKind::continuation;
    return {kind, SfsId{(encoded - 2U) / 2U}};
}

void claim_cluster(std::span<std::uint32_t> owners, std::uint32_t cluster, SfsId record, AllocationClaimKind kind,
                   AllocationSummary &summary, std::size_t conflict_limit) {
    const auto claimed = record_owner(record, kind);
    auto &existing = owners[cluster];
    if (existing == 0U) {
        existing = claimed;
        return;
    }
    if ((existing & owner_conflict_bit) == 0U)
        ++summary.conflicting_cluster_count;
    if (summary.conflicts.size() < conflict_limit) {
        summary.conflicts.push_back({cluster, decode_owner(existing), {kind, record}});
    } else {
        summary.conflicts_truncated = true;
    }
    existing |= owner_conflict_bit;
}

std::vector<AllocationMismatchRange> mismatch_ranges(std::span<const std::byte> left, std::span<const std::byte> right,
                                                     std::uint32_t cluster_count, std::size_t limit) {
    std::vector<AllocationMismatchRange> result;
    std::optional<std::uint32_t> start;
    for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
        const bool differs = bitmap_test(left, cluster) && !bitmap_test(right, cluster);
        if (differs && !start) {
            start = cluster;
        } else if (!differs && start) {
            if (result.size() < limit) {
                result.push_back({*start, cluster - 1U});
            }
            start.reset();
        }
    }
    if (start && result.size() < limit) {
        result.push_back({*start, cluster_count - 1U});
    }
    return result;
}

std::uint32_t mismatch_cluster_count(std::span<const std::byte> left, std::span<const std::byte> right,
                                     std::uint32_t cluster_count) {
    std::uint32_t result{};
    for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
        if (bitmap_test(left, cluster) && !bitmap_test(right, cluster))
            ++result;
    }
    return result;
}

std::optional<ParsedRecord> parse_record_header(std::span<const std::byte> bytes) {
    if (bytes.size() != index_record_size) {
        return std::nullopt;
    }
    const ByteReader reader{bytes};
    const auto extent_count = reader.be16(0);
    const auto reserved = reader.be16(2);
    const auto cluster_count = reader.be16(4);
    const auto data_size = reader.be32(6);
    if (!extent_count || !reserved || !cluster_count || !data_size || *extent_count == 0 || *reserved != 0 ||
        *cluster_count == 0 || *data_size == 0) {
        return std::nullopt;
    }
    return ParsedRecord{*extent_count, *cluster_count, *data_size};
}

Result<std::vector<Extent>> direct_extents(std::span<const std::byte> bytes, std::uint16_t extent_count) {
    std::vector<Extent> result;
    const ByteReader reader{bytes};
    for (std::uint16_t index = 0; index < extent_count; ++index) {
        const auto offset = 0x0aU + static_cast<std::size_t>(index) * 12U;
        const auto cluster = reader.be32(offset);
        const auto count = reader.be32(offset + 4U);
        const auto bytes_count = reader.be32(offset + 8U);
        if (!cluster || !count || !bytes_count) {
            return std::unexpected{!cluster ? cluster.error() : (!count ? count.error() : bytes_count.error())};
        }
        if (*cluster == 0 || *count == 0 || *bytes_count == 0) {
            return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                              "direct extent contains a zero field")};
        }
        result.push_back({*cluster, *count, *bytes_count});
    }
    return result;
}

Result<std::vector<Extent>> continuation_extents(const RandomAccessReader &image, const Partition &partition,
                                                 std::uint32_t sector_size, std::uint32_t list_cluster,
                                                 std::uint16_t expected_count,
                                                 std::vector<std::uint32_t> &list_clusters,
                                                 const OpenOptions &options) {
    std::vector<Extent> result;
    std::unordered_set<std::uint32_t> seen;
    const auto cluster_bytes_u64 = checked_multiply(sector_size, partition.sectors_per_cluster);
    if (!cluster_bytes_u64 || *cluster_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "cluster size is outside the supported memory range")};
    }
    const auto cluster_bytes = static_cast<std::size_t>(*cluster_bytes_u64);
    const auto max_triplets = (cluster_bytes - continuation_header_size) / 12U;
    while (list_cluster != 0 && result.size() < expected_count) {
        if (list_cluster >= partition.cluster_count) {
            return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                              "continuation-list cluster is outside the partition")};
        }
        if (!seen.insert(list_cluster).second) {
            return std::unexpected{make_error(ErrorCode::allocation_cycle, ErrorCategory::allocation,
                                              "continuation-list extent cycle detected")};
        }
        list_clusters.push_back(list_cluster);
        const auto offset =
            cluster_offset(partition.start_sector, sector_size, partition.sectors_per_cluster, list_cluster);
        if (!offset) {
            return std::unexpected{offset.error()};
        }
        const auto payload = read_bytes(image, *offset, cluster_bytes, options.cancellation);
        if (!payload) {
            return std::unexpected{payload.error()};
        }
        const ByteReader reader{*payload};
        const auto block_count = reader.be32(0);
        const auto next_cluster = reader.be32(8);
        if (!block_count || !next_cluster || *block_count == 0 || *block_count > max_triplets ||
            *block_count > expected_count - result.size()) {
            return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                              "continuation-list extent count is invalid")};
        }
        for (std::uint32_t index = 0; index < *block_count; ++index) {
            const auto item_offset = continuation_header_size + index * 12U;
            const auto cluster = reader.be32(item_offset);
            const auto count = reader.be32(item_offset + 4U);
            const auto byte_count = reader.be32(item_offset + 8U);
            if (!cluster || !count || !byte_count || *cluster == 0 || *count == 0 || *byte_count == 0) {
                return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                                  "continuation-list extent triplet is invalid")};
            }
            result.push_back({*cluster, *count, *byte_count});
        }
        list_cluster = *next_cluster;
    }
    if (result.size() != expected_count) {
        return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                          "continuation list does not resolve every extent")};
    }
    return result;
}

Result<std::vector<std::byte>> read_logical_prefix(const RandomAccessReader &image, const Partition &partition,
                                                   std::uint32_t sector_size, const IndexRecord &record,
                                                   std::size_t limit, const OpenOptions &options) {
    const auto wanted = std::min<std::uint64_t>(record.data_size, limit);
    std::vector<std::byte> result;
    result.reserve(static_cast<std::size_t>(wanted));
    for (const auto &extent : record.extents) {
        if (result.size() >= wanted) {
            break;
        }
        const auto capacity = checked_multiply(extent.cluster_count,
                                               static_cast<std::uint64_t>(sector_size) * partition.sectors_per_cluster);
        if (!capacity || extent.byte_count > *capacity || extent.cluster_offset >= partition.cluster_count ||
            extent.cluster_count > partition.cluster_count - extent.cluster_offset) {
            return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                              "data extent exceeds its allocation or partition")};
        }
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>({extent.byte_count, *capacity, wanted - result.size()}));
        const auto offset =
            cluster_offset(partition.start_sector, sector_size, partition.sectors_per_cluster, extent.cluster_offset);
        if (!offset) {
            return std::unexpected{offset.error()};
        }
        const auto part = read_bytes(image, *offset, count, options.cancellation);
        if (!part) {
            return std::unexpected{part.error()};
        }
        result.insert(result.end(), part->begin(), part->end());
    }
    return result;
}

Result<std::vector<std::byte>> read_logical_range(const RandomAccessReader &image, const Partition &partition,
                                                  std::uint32_t sector_size, const IndexRecord &record,
                                                  std::uint64_t requested_offset, std::size_t requested_size,
                                                  const OpenOptions &options) {
    if (requested_offset > record.data_size || requested_size > record.data_size - requested_offset) {
        return std::unexpected{make_error(ErrorCode::out_of_bounds, ErrorCategory::object,
                                          "logical SFS record range exceeds its declared size")};
    }
    std::vector<std::byte> result;
    result.reserve(requested_size);
    std::uint64_t logical_offset{};
    for (const auto &extent : record.extents) {
        const auto capacity = checked_multiply(extent.cluster_count,
                                               static_cast<std::uint64_t>(sector_size) * partition.sectors_per_cluster);
        if (!capacity || extent.byte_count > *capacity || extent.cluster_offset >= partition.cluster_count ||
            extent.cluster_count > partition.cluster_count - extent.cluster_offset) {
            return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                              "data extent exceeds its allocation or partition")};
        }
        const auto extent_end = logical_offset + extent.byte_count;
        if (requested_offset < extent_end && result.size() < requested_size) {
            const auto within_extent = requested_offset > logical_offset ? requested_offset - logical_offset : 0U;
            const auto available = extent.byte_count - within_extent;
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(available, requested_size - result.size()));
            const auto physical = cluster_offset(partition.start_sector, sector_size, partition.sectors_per_cluster,
                                                 extent.cluster_offset);
            if (!physical)
                return std::unexpected{physical.error()};
            auto part = read_bytes(image, *physical + within_extent, count, options.cancellation);
            if (!part)
                return std::unexpected{part.error()};
            result.insert(result.end(), part->begin(), part->end());
            requested_offset += count;
        }
        logical_offset = extent_end;
        if (result.size() == requested_size)
            break;
    }
    if (result.size() != requested_size) {
        return std::unexpected{
            make_error(ErrorCode::io_short_read, ErrorCategory::io, "logical SFS record range is truncated")};
    }
    return result;
}

Result<Partition> parse_partition(const RandomAccessReader &image, const PartitionEntry &table_entry,
                                  std::uint32_t sector_size, const OpenOptions &options) {
    Partition result;
    result.index = table_entry.index;
    result.start_sector = table_entry.start_sector;
    result.sector_count = table_entry.sector_count;
    const auto start = checked_multiply(table_entry.start_sector, sector_size);
    if (!start) {
        return std::unexpected{start.error()};
    }
    const auto header =
        read_bytes(image, *start, static_cast<std::size_t>(partition_header_size), options.cancellation);
    if (!header) {
        return std::unexpected{header.error()};
    }
    if (!begins_with(*header, magic)) {
        return std::unexpected{partition_error(ErrorCode::container_unrecognized,
                                               "partition header does not contain Yamaha SFS magic", result.index,
                                               *start)};
    }
    const auto backup_offset = checked_add(*start, partition_header_size);
    if (!backup_offset) {
        return std::unexpected{backup_offset.error()};
    }
    const auto backup =
        read_bytes(image, *backup_offset, static_cast<std::size_t>(partition_header_size), options.cancellation);
    if (!backup) {
        return std::unexpected{backup.error()};
    }
    result.backup_header_matches = *header == *backup;
    if (!result.backup_header_matches) {
        result.diagnostics.push_back(partition_error(ErrorCode::container_backup_mismatch,
                                                     "backup partition header differs from primary", result.index,
                                                     *backup_offset));
    }
    const ByteReader reader{*header};
    const auto name = reader.ascii_field(0x40, 16);
    const auto cluster_count = reader.be32(0x90);
    const auto sectors_per_cluster = reader.be32(0x94);
    const auto bitmap_cluster = reader.be32(0x9c);
    const auto index_cluster = reader.be32(0xa4);
    const auto index_span = reader.be32(0xa8);
    if (!name || !cluster_count || !sectors_per_cluster || !bitmap_cluster || !index_cluster || !index_span ||
        *cluster_count == 0 || *sectors_per_cluster == 0 || *index_span == 0) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition contains incomplete or zero SFS geometry", result.index,
                                               *start)};
    }
    result.name = *name;
    result.cluster_count = *cluster_count;
    result.sectors_per_cluster = *sectors_per_cluster;
    result.bitmap_cluster = *bitmap_cluster;
    result.directory_index_cluster = *index_cluster;
    result.directory_index_span_clusters = *index_span;
    std::copy_n(header->begin() + 0xac, result.unresolved_header_tail.size(), result.unresolved_header_tail.begin());

    const auto physical_cluster_capacity = static_cast<std::uint64_t>(result.sector_count) / result.sectors_per_cluster;
    if (result.cluster_count > physical_cluster_capacity) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition cluster count exceeds its physical sector capacity",
                                               result.index, *start + 0x90U)};
    }

    const auto cluster_bytes = checked_multiply(sector_size, result.sectors_per_cluster);
    const auto raw_index_bytes = cluster_bytes ? checked_multiply(*cluster_bytes, result.directory_index_span_clusters)
                                               : Result<std::uint64_t>{std::unexpected{cluster_bytes.error()}};
    if (!cluster_bytes || !raw_index_bytes || *raw_index_bytes > options.max_index_bytes ||
        *raw_index_bytes > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition index span exceeds configured bounds", result.index,
                                               *start + 0xa8U)};
    }
    const auto index_end = checked_add(result.directory_index_cluster, result.directory_index_span_clusters);
    if (!index_end || *index_end > result.cluster_count) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition index extends beyond the cluster range", result.index,
                                               *start + 0xa4U)};
    }
    const auto bitmap_layout = detail::sfs_allocation_bitmap_layout(
        result.start_sector, result.cluster_count, result.sectors_per_cluster, result.bitmap_cluster, sector_size);
    if (!bitmap_layout || bitmap_layout->useful_bytes > std::numeric_limits<std::size_t>::max() ||
        bitmap_layout->rounded_bytes > std::numeric_limits<std::size_t>::max() ||
        bitmap_layout->rounded_bytes > options.max_allocation_bitmap_bytes) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition bitmap exceeds the configured memory bound", result.index,
                                               *start + 0x9cU)};
    }
    const auto bitmap_end = checked_add(result.bitmap_cluster, bitmap_layout->span_clusters);
    if (!bitmap_end || *bitmap_end > result.cluster_count) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition bitmap extends beyond the cluster range", result.index,
                                               *start + 0x9cU)};
    }
    const auto allocation_regions_are_disjoint =
        *bitmap_end <= result.directory_index_cluster || *index_end <= result.bitmap_cluster;
    if (!allocation_regions_are_disjoint) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition bitmap overlaps the directory index", result.index,
                                               *start + 0x9cU)};
    }
    const auto index_offset =
        cluster_offset(result.start_sector, sector_size, result.sectors_per_cluster, result.directory_index_cluster);
    if (!index_offset) {
        return std::unexpected{index_offset.error()};
    }
    const auto index_data =
        read_bytes(image, *index_offset, static_cast<std::size_t>(*raw_index_bytes), options.cancellation);
    if (!index_data) {
        return std::unexpected{index_data.error()};
    }

    std::vector<std::byte> reconstructed(static_cast<std::size_t>(bitmap_layout->useful_bytes));
    const auto owner_bytes = checked_multiply(result.cluster_count, sizeof(std::uint32_t));
    if (!owner_bytes || *owner_bytes > std::numeric_limits<std::size_t>::max() ||
        *owner_bytes > options.max_allocation_owner_bytes) {
        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                               "partition allocation ownership exceeds the configured memory bound",
                                               result.index)};
    }
    const auto first_payload_u64 = index_end;
    const auto first_payload = static_cast<std::uint32_t>(*first_payload_u64);
    std::vector<std::uint32_t> owners(result.cluster_count);
    std::ranges::fill(std::span{owners}.first(first_payload), reserved_owner);
    for (std::size_t block = 0; block + index_block_size <= index_data->size(); block += index_block_size) {
        if (const auto check = options.cancellation.check(); !check) {
            return std::unexpected{check.error()};
        }
        for (std::size_t slot = 0; slot < records_per_index_block; ++slot) {
            const auto relative = block + slot * index_record_size;
            const auto bytes =
                std::span<const std::byte>{*index_data}.subspan(relative, static_cast<std::size_t>(index_record_size));
            if (std::all_of(bytes.begin(), bytes.begin() + 4, [](std::byte value) { return value == std::byte{}; })) {
                continue;
            }
            const auto parsed = parse_record_header(bytes);
            if (!parsed) {
                result.diagnostics.push_back(partition_error(ErrorCode::object_malformed,
                                                             "nonempty SFS index slot has an invalid record header",
                                                             result.index, *index_offset + relative));
                continue;
            }
            IndexRecord record;
            record.sfs_id =
                SfsId{static_cast<std::uint32_t>((block / index_block_size) * records_per_index_block + slot)};
            record.record_offset = ByteOffset{*index_offset + relative};
            record.extent_count = parsed->extent_count;
            record.cluster_count = parsed->cluster_count;
            record.data_size = parsed->data_size;
            Result<std::vector<Extent>> extents = std::unexpected{make_error(
                ErrorCode::allocation_invalid_extent, ErrorCategory::allocation, "extent parser was not selected")};
            if (record.extent_count <= direct_extent_limit) {
                extents = direct_extents(bytes, record.extent_count);
            } else {
                const ByteReader record_reader{bytes};
                const auto list_cluster = record_reader.be32(0x0a);
                if (list_cluster) {
                    extents = continuation_extents(image, result, sector_size, *list_cluster, record.extent_count,
                                                   record.continuation_clusters, options);
                }
            }
            if (!extents) {
                ++result.allocation.invalid_extent_record_count;
                auto diagnostic = extents.error();
                diagnostic.context.partition_index = result.index.value;
                diagnostic.context.raw_offset = record.record_offset.value;
                result.diagnostics.push_back(std::move(diagnostic));
                result.records.push_back(std::move(record));
                continue;
            }
            record.extents = std::move(*extents);
            std::uint64_t extent_cluster_sum{};
            std::uint64_t extent_byte_sum{};
            for (const auto list_cluster : record.continuation_clusters) {
                if (list_cluster < result.cluster_count) {
                    claim_cluster(owners, list_cluster, record.sfs_id, AllocationClaimKind::continuation,
                                  result.allocation, options.max_allocation_conflicts);
                    if (!bitmap_set(reconstructed, list_cluster)) {
                        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                                               "continuation cluster exceeds the partition bitmap",
                                                               result.index, record.record_offset.value)};
                    }
                }
            }
            for (const auto &extent : record.extents) {
                extent_cluster_sum += extent.cluster_count;
                extent_byte_sum += extent.byte_count;
                if (extent.cluster_offset >= result.cluster_count ||
                    extent.cluster_count > result.cluster_count - extent.cluster_offset) {
                    ++result.allocation.invalid_extent_record_count;
                    result.diagnostics.push_back(partition_error(ErrorCode::allocation_invalid_extent,
                                                                 "SFS extent lies outside the partition cluster range",
                                                                 result.index, record.record_offset.value));
                    continue;
                }
                for (std::uint32_t cluster = extent.cluster_offset;
                     cluster < extent.cluster_offset + extent.cluster_count; ++cluster) {
                    claim_cluster(owners, cluster, record.sfs_id, AllocationClaimKind::data, result.allocation,
                                  options.max_allocation_conflicts);
                    if (!bitmap_set(reconstructed, cluster)) {
                        return std::unexpected{partition_error(ErrorCode::container_invalid_geometry,
                                                               "data cluster exceeds the partition bitmap",
                                                               result.index, record.record_offset.value)};
                    }
                }
            }
            if (extent_cluster_sum != record.cluster_count) {
                ++result.allocation.extent_total_mismatch_count;
            }
            record.extent_byte_count_total = extent_byte_sum;
            if (extent_byte_sum != record.data_size) {
                ++result.allocation.extent_byte_total_mismatch_count;
                result.diagnostics.push_back(
                    partition_error(ErrorCode::allocation_mismatch,
                                    std::format("SFS ID {} extent byte total {} differs from logical size {}",
                                                record.sfs_id.value, extent_byte_sum, record.data_size),
                                    result.index, record.record_offset.value));
            }
            auto prefix = read_logical_prefix(image, result, sector_size, record, 0x200U, options);
            if (prefix) {
                classify_record(record, *prefix);
                if (record.payload_kind == PayloadKind::directory && record.data_size > prefix->size() &&
                    record.data_size <= options.max_directory_bytes) {
                    const auto expanded = read_logical_prefix(image, result, sector_size, record,
                                                              static_cast<std::size_t>(record.data_size), options);
                    if (expanded) {
                        record.directory_entries = parse_directory_entries(*expanded);
                    } else {
                        auto diagnostic = expanded.error();
                        diagnostic.context.partition_index = result.index.value;
                        diagnostic.context.raw_offset = record.record_offset.value;
                        result.diagnostics.push_back(std::move(diagnostic));
                    }
                }
            } else {
                auto diagnostic = prefix.error();
                diagnostic.context.partition_index = result.index.value;
                diagnostic.context.raw_offset = record.record_offset.value;
                result.diagnostics.push_back(std::move(diagnostic));
            }
            result.records.push_back(std::move(record));
        }
    }

    if (result.allocation.conflicting_cluster_count != 0U) {
        result.diagnostics.push_back(partition_error(
            ErrorCode::allocation_cross_link,
            std::format("{} cluster(s) have multiple allocation owners", result.allocation.conflicting_cluster_count),
            result.index));
    }

    if (const auto directories = validate_directory_graph(result, options); !directories) {
        return std::unexpected{directories.error()};
    }
    const auto header_addressed =
        read_bytes(image, bitmap_layout->header_addressed_offset,
                   static_cast<std::size_t>(bitmap_layout->rounded_bytes), options.cancellation);
    const auto fixed_location =
        read_bytes(image, bitmap_layout->fixed_location_offset, static_cast<std::size_t>(bitmap_layout->rounded_bytes),
                   options.cancellation);
    if (!header_addressed || !fixed_location) {
        return std::unexpected{header_addressed ? fixed_location.error() : header_addressed.error()};
    }
    const auto useful_size = static_cast<std::size_t>(bitmap_layout->useful_bytes);
    const auto header_useful = std::span<const std::byte>{*header_addressed}.first(useful_size);
    const auto fixed_useful = std::span<const std::byte>{*fixed_location}.first(useful_size);
    result.allocation.stored_copies_match = *header_addressed == *fixed_location;
    for (std::size_t index = 0U; index < header_addressed->size(); ++index) {
        if ((*header_addressed)[index] != (*fixed_location)[index])
            ++result.allocation.stored_copy_mismatch_byte_count;
    }
    result.allocation.header_addressed.used_cluster_count = count_bitmap_bits(header_useful, result.cluster_count);
    result.allocation.fixed_location.used_cluster_count = count_bitmap_bits(fixed_useful, result.cluster_count);
    result.allocation.reconstructed_used_cluster_count = count_bitmap_bits(reconstructed, result.cluster_count);
    result.allocation.header_addressed.marked_used_without_index_extent_count =
        mismatch_cluster_count(header_useful, reconstructed, result.cluster_count);
    result.allocation.header_addressed.index_extent_marked_free_count =
        mismatch_cluster_count(reconstructed, header_useful, result.cluster_count);
    result.allocation.fixed_location.marked_used_without_index_extent_count =
        mismatch_cluster_count(fixed_useful, reconstructed, result.cluster_count);
    result.allocation.fixed_location.index_extent_marked_free_count =
        mismatch_cluster_count(reconstructed, fixed_useful, result.cluster_count);
    result.allocation.header_addressed.marked_used_without_index_extent =
        mismatch_ranges(header_useful, reconstructed, result.cluster_count, options.max_mismatch_ranges);
    result.allocation.header_addressed.index_extent_marked_free =
        mismatch_ranges(reconstructed, header_useful, result.cluster_count, options.max_mismatch_ranges);
    result.allocation.fixed_location.marked_used_without_index_extent =
        mismatch_ranges(fixed_useful, reconstructed, result.cluster_count, options.max_mismatch_ranges);
    result.allocation.fixed_location.index_extent_marked_free =
        mismatch_ranges(reconstructed, fixed_useful, result.cluster_count, options.max_mismatch_ranges);
    result.allocation.header_not_fixed =
        mismatch_ranges(header_useful, fixed_useful, result.cluster_count, options.max_mismatch_ranges);
    result.allocation.fixed_not_header =
        mismatch_ranges(fixed_useful, header_useful, result.cluster_count, options.max_mismatch_ranges);
    if (first_payload_u64 && *first_payload_u64 <= std::numeric_limits<std::uint32_t>::max() &&
        *cluster_bytes <= std::numeric_limits<std::uint32_t>::max()) {
        const auto free = calculate_sfs_free_space(result.cluster_count, static_cast<std::uint32_t>(*first_payload_u64),
                                                   result.allocation.header_addressed.used_cluster_count,
                                                   static_cast<std::uint32_t>(*cluster_bytes));
        if (free) {
            result.allocation.free_space = *free;
        } else {
            result.diagnostics.push_back(free.error());
        }
    }
    return result;
}

} // namespace axk::sfs_detail
