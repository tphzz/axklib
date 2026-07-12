#include "axklib/sfs.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>
#include <unordered_map>

#include "axklib/bytes.hpp"
#include "axklib/utf8.hpp"

namespace axk {
namespace {

constexpr std::string_view magic{"YAMAHA_dev3"};
constexpr std::string_view object_magic{"FSFSDEV3SPLX"};
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

Result<std::vector<std::byte>> read_bytes(
    const RandomAccessReader& reader,
    std::uint64_t offset,
    std::size_t count,
    const CancellationToken& cancellation) {
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

Result<std::uint64_t> cluster_offset(
    std::uint32_t partition_start_sector,
    std::uint32_t sector_size,
    std::uint32_t sectors_per_cluster,
    std::uint32_t cluster) {
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

Error partition_error(
    ErrorCode code,
    std::string message,
    PartitionIndex index,
    std::uint64_t offset = 0) {
  ErrorContext context;
  context.partition_index = index.value;
  if (offset != 0) {
    context.raw_offset = offset;
  }
  return make_error(code, ErrorCategory::container, std::move(message), std::move(context));
}

Result<Superblock> parse_superblock(std::span<const std::byte> bytes) {
  if (bytes.size() != sfs_default_sector_size) {
    return std::unexpected{make_error(
        ErrorCode::container_truncated,
        ErrorCategory::container,
        "SFS superblock must contain exactly 512 bytes")};
  }
  if (!begins_with(bytes, magic)) {
    return std::unexpected{make_error(
        ErrorCode::container_unrecognized,
        ErrorCategory::container,
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
    result.partition_entries[index] = {
        PartitionIndex{static_cast<std::uint8_t>(index)}, *start, *count};
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
  return (std::to_integer<std::uint8_t>(bitmap[cluster / 8U]) &
          static_cast<std::uint8_t>(0x80U >> (cluster & 7U))) != 0;
}

void bitmap_set(std::span<std::byte> bitmap, std::uint32_t cluster) {
  const auto index = cluster / 8U;
  bitmap[index] |= static_cast<std::byte>(0x80U >> (cluster & 7U));
}

std::vector<AllocationMismatchRange> mismatch_ranges(
    std::span<const std::byte> left,
    std::span<const std::byte> right,
    std::uint32_t cluster_count,
    std::size_t limit) {
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

std::optional<ParsedRecord> parse_record_header(std::span<const std::byte> bytes) {
  if (bytes.size() != index_record_size) {
    return std::nullopt;
  }
  const ByteReader reader{bytes};
  const auto extent_count = reader.be16(0);
  const auto reserved = reader.be16(2);
  const auto cluster_count = reader.be16(4);
  const auto data_size = reader.be32(6);
  if (!extent_count || !reserved || !cluster_count || !data_size || *extent_count == 0 ||
      *reserved != 0 || *cluster_count == 0 || *data_size == 0) {
    return std::nullopt;
  }
  return ParsedRecord{*extent_count, *cluster_count, *data_size};
}

Result<std::vector<Extent>> direct_extents(
    std::span<const std::byte> bytes,
    std::uint16_t extent_count) {
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
      return std::unexpected{make_error(
          ErrorCode::allocation_invalid_extent,
          ErrorCategory::allocation,
          "direct extent contains a zero field")};
    }
    result.push_back({*cluster, *count, *bytes_count});
  }
  return result;
}

Result<std::vector<Extent>> continuation_extents(
    const RandomAccessReader& image,
    const Partition& partition,
    std::uint32_t sector_size,
    std::uint32_t list_cluster,
    std::uint16_t expected_count,
    std::vector<std::uint32_t>& list_clusters,
    const OpenOptions& options) {
  std::vector<Extent> result;
  std::unordered_set<std::uint32_t> seen;
  const auto cluster_bytes_u64 = checked_multiply(sector_size, partition.sectors_per_cluster);
  if (!cluster_bytes_u64 || *cluster_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
    return std::unexpected{make_error(
        ErrorCode::container_invalid_geometry,
        ErrorCategory::container,
        "cluster size is outside the supported memory range")};
  }
  const auto cluster_bytes = static_cast<std::size_t>(*cluster_bytes_u64);
  const auto max_triplets = (cluster_bytes - continuation_header_size) / 12U;
  while (list_cluster != 0 && result.size() < expected_count) {
    if (list_cluster >= partition.cluster_count) {
      return std::unexpected{make_error(
          ErrorCode::allocation_invalid_extent,
          ErrorCategory::allocation,
          "continuation-list cluster is outside the partition")};
    }
    if (!seen.insert(list_cluster).second) {
      return std::unexpected{make_error(
          ErrorCode::allocation_cycle,
          ErrorCategory::allocation,
          "continuation-list extent cycle detected")};
    }
    list_clusters.push_back(list_cluster);
    const auto offset = cluster_offset(
        partition.start_sector, sector_size, partition.sectors_per_cluster, list_cluster);
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
      return std::unexpected{make_error(
          ErrorCode::allocation_invalid_extent,
          ErrorCategory::allocation,
          "continuation-list extent count is invalid")};
    }
    for (std::uint32_t index = 0; index < *block_count; ++index) {
      const auto item_offset = continuation_header_size + index * 12U;
      const auto cluster = reader.be32(item_offset);
      const auto count = reader.be32(item_offset + 4U);
      const auto byte_count = reader.be32(item_offset + 8U);
      if (!cluster || !count || !byte_count || *cluster == 0 || *count == 0 ||
          *byte_count == 0) {
        return std::unexpected{make_error(
            ErrorCode::allocation_invalid_extent,
            ErrorCategory::allocation,
            "continuation-list extent triplet is invalid")};
      }
      result.push_back({*cluster, *count, *byte_count});
    }
    list_cluster = *next_cluster;
  }
  if (result.size() != expected_count) {
    return std::unexpected{make_error(
        ErrorCode::allocation_invalid_extent,
        ErrorCategory::allocation,
        "continuation list does not resolve every extent")};
  }
  return result;
}

Result<std::vector<std::byte>> read_logical_prefix(
    const RandomAccessReader& image,
    const Partition& partition,
    std::uint32_t sector_size,
    const IndexRecord& record,
    std::size_t limit,
    const OpenOptions& options) {
  const auto wanted = std::min<std::uint64_t>(record.data_size, limit);
  std::vector<std::byte> result;
  result.reserve(static_cast<std::size_t>(wanted));
  for (const auto& extent : record.extents) {
    if (result.size() >= wanted) {
      break;
    }
    const auto capacity = checked_multiply(
        extent.cluster_count,
        static_cast<std::uint64_t>(sector_size) * partition.sectors_per_cluster);
    if (!capacity || extent.byte_count > *capacity ||
        extent.cluster_offset >= partition.cluster_count ||
        extent.cluster_count > partition.cluster_count - extent.cluster_offset) {
      return std::unexpected{make_error(
          ErrorCode::allocation_invalid_extent,
          ErrorCategory::allocation,
          "data extent exceeds its allocation or partition")};
    }
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
        {extent.byte_count, *capacity, wanted - result.size()}));
    const auto offset = cluster_offset(
        partition.start_sector,
        sector_size,
        partition.sectors_per_cluster,
        extent.cluster_offset);
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

std::vector<DirectoryEntry> parse_directory_entries(std::span<const std::byte> payload) {
  std::vector<DirectoryEntry> result;
  const ByteReader reader{payload};
  for (std::size_t offset = 0; offset + 32U <= payload.size(); offset += 32U) {
    const auto prefix = reader.be32(offset);
    const auto link = reader.be32(offset + 4U);
    if (!prefix || !link || (*prefix == 0 && *link == 0)) {
      break;
    }
    const auto flags = reader.be16(offset);
    const auto name_size = reader.be16(offset + 2U);
    if (!flags || !name_size || *name_size == 0 || *name_size > 24U) {
      break;
    }
    const auto name = reader.ascii_field(offset + 8U, *name_size, false);
    if (!name) {
      break;
    }
    result.push_back({*flags, LinkId{*link}, *name, offset});
  }
  return result;
}

void classify_record(IndexRecord& record, std::span<const std::byte> payload) {
  if (begins_with(payload, object_magic) && payload.size() >= 0x42U) {
    const ByteReader reader{payload};
    const auto type = reader.ascii_field(0x0c, 4);
    const auto name = reader.ascii_field(0x32, 16);
    if (type && name) {
      record.payload_kind = PayloadKind::object;
      record.object_type = *type;
      record.object_name = *name;
    }
    return;
  }
  if (payload.size() >= 16U) {
    bool marker_lane = true;
    for (std::size_t offset = 1; offset < 16U; offset += 2U) {
      const auto expected = offset % 4U == 1U ? 0x55U : 0xaaU;
      marker_lane &= std::to_integer<std::uint8_t>(payload[offset]) == expected;
    }
    constexpr std::array<std::byte, 6> even_magic{
        std::byte{'F'}, std::byte{'F'}, std::byte{'D'},
        std::byte{'V'}, std::byte{'S'}, std::byte{'L'}};
    for (std::size_t index = 0; index < even_magic.size(); ++index) {
      marker_lane &= payload[index * 2U] == even_magic[index];
    }
    if (marker_lane) {
      const std::array type_code{payload[12], payload[14]};
      constexpr std::array mappings{
          std::pair{std::array{std::byte{'S'}, std::byte{'P'}}, std::string_view{"SMPL"}},
          std::pair{std::array{std::byte{'S'}, std::byte{'N'}}, std::string_view{"SBNK"}},
          std::pair{std::array{std::byte{'S'}, std::byte{'A'}}, std::string_view{"SBAC"}},
          std::pair{std::array{std::byte{'P'}, std::byte{'O'}}, std::string_view{"PROG"}},
          std::pair{std::array{std::byte{'S'}, std::byte{'Q'}}, std::string_view{"SEQU"}},
          std::pair{std::array{std::byte{'P'}, std::byte{'F'}}, std::string_view{"PRF3"}},
      };
      const auto mapping = std::find_if(mappings.begin(), mappings.end(), [&](const auto& item) {
        return item.first == type_code;
      });
      if (mapping != mappings.end()) {
        record.payload_kind = PayloadKind::alternating_byte_object;
        record.object_type = mapping->second;
        return;
      }
    }
  }
  auto entries = parse_directory_entries(payload);
  if (entries.size() >= 2U && entries[0].name == "." && entries[1].name == "..") {
    record.payload_kind = PayloadKind::directory;
    record.directory_id = entries[0].link_id;
    record.parent_directory_id = entries[1].link_id;
    record.directory_entries = std::move(entries);
  }
}

void validate_directory_graph(Partition& partition, const OpenOptions& options) {
  std::unordered_map<std::uint32_t, const IndexRecord*> records_by_id;
  std::unordered_map<std::uint32_t, const IndexRecord*> directories_by_id;
  for (const auto& record : partition.records) {
    records_by_id.emplace(record.sfs_id.value, &record);
    if (!record.directory_id) {
      continue;
    }
    const bool inserted = directories_by_id.emplace(record.directory_id->value, &record).second;
    if (!inserted) {
      ErrorContext context;
      context.partition_index = partition.index.value;
      context.raw_offset = record.record_offset.value;
      partition.diagnostics.push_back(make_error(
          ErrorCode::relationship_ambiguous,
          ErrorCategory::relationship,
          "multiple SFS directory records claim the same directory ID",
          std::move(context)));
    }
    if (record.data_size > options.max_directory_bytes) {
      ErrorContext context;
      context.partition_index = partition.index.value;
      context.raw_offset = record.record_offset.value;
      partition.diagnostics.push_back(make_error(
          ErrorCode::object_malformed,
          ErrorCategory::object,
          "directory payload exceeds the configured traversal bound",
          std::move(context)));
    }
  }

  for (const auto& [directory_id, directory] : directories_by_id) {
    static_cast<void>(directory_id);
    for (const auto& entry : directory->directory_entries) {
      if (entry.name == ".") {
        continue;
      }
      if (!records_by_id.contains(entry.link_id.value)) {
        ErrorContext context;
        context.partition_index = partition.index.value;
        context.object_type = "directory-entry";
        context.object_name = entry.name;
        context.raw_offset = directory->record_offset.value + entry.payload_relative_offset;
        partition.diagnostics.push_back(make_error(
            ErrorCode::relationship_unresolved,
            ErrorCategory::relationship,
            "directory entry references a missing SFS record",
            std::move(context)));
      }
    }
  }

  std::unordered_map<std::uint32_t, std::uint8_t> colors;
  std::function<void(std::uint32_t)> visit = [&](std::uint32_t directory_id) {
    colors[directory_id] = 1;
    const auto found = directories_by_id.find(directory_id);
    if (found == directories_by_id.end()) {
      colors[directory_id] = 2;
      return;
    }
    for (const auto& entry : found->second->directory_entries) {
      if (entry.name == "." || entry.name == ".." ||
          !directories_by_id.contains(entry.link_id.value)) {
        continue;
      }
      const auto color = colors[entry.link_id.value];
      if (color == 1) {
        ErrorContext context;
        context.partition_index = partition.index.value;
        context.object_type = "directory-entry";
        context.object_name = entry.name;
        context.raw_offset = found->second->record_offset.value + entry.payload_relative_offset;
        partition.diagnostics.push_back(make_error(
            ErrorCode::relationship_cycle,
            ErrorCategory::relationship,
            "directory child links contain a cycle",
            std::move(context)));
      } else if (color == 0) {
        visit(entry.link_id.value);
      }
    }
    colors[directory_id] = 2;
  };
  for (const auto& item : directories_by_id) {
    const auto directory_id = item.first;
    if (colors[directory_id] == 0) {
      visit(directory_id);
    }
  }
}

Result<Partition> parse_partition(
    const RandomAccessReader& image,
    const PartitionEntry& table_entry,
    std::uint32_t sector_size,
    const OpenOptions& options) {
  Partition result;
  result.index = table_entry.index;
  result.start_sector = table_entry.start_sector;
  result.sector_count = table_entry.sector_count;
  const auto start = checked_multiply(table_entry.start_sector, sector_size);
  if (!start) {
    return std::unexpected{start.error()};
  }
  const auto header = read_bytes(
      image, *start, static_cast<std::size_t>(partition_header_size), options.cancellation);
  if (!header) {
    return std::unexpected{header.error()};
  }
  if (!begins_with(*header, magic)) {
    return std::unexpected{partition_error(
        ErrorCode::container_unrecognized,
        "partition header does not contain Yamaha SFS magic",
        result.index,
        *start)};
  }
  const auto backup_offset = checked_add(*start, partition_header_size);
  if (!backup_offset) {
    return std::unexpected{backup_offset.error()};
  }
  const auto backup = read_bytes(
      image, *backup_offset, static_cast<std::size_t>(partition_header_size), options.cancellation);
  if (!backup) {
    return std::unexpected{backup.error()};
  }
  result.backup_header_matches = *header == *backup;
  if (!result.backup_header_matches) {
    result.diagnostics.push_back(partition_error(
        ErrorCode::container_backup_mismatch,
        "backup partition header differs from primary",
        result.index,
        *backup_offset));
  }
  const ByteReader reader{*header};
  const auto name = reader.ascii_field(0x40, 16);
  const auto cluster_count = reader.be32(0x90);
  const auto sectors_per_cluster = reader.be32(0x94);
  const auto bitmap_cluster = reader.be32(0x9c);
  const auto index_cluster = reader.be32(0xa4);
  const auto index_span = reader.be32(0xa8);
  if (!name || !cluster_count || !sectors_per_cluster || !bitmap_cluster || !index_cluster ||
      !index_span || *cluster_count == 0 || *sectors_per_cluster == 0 || *index_span == 0) {
    return std::unexpected{partition_error(
        ErrorCode::container_invalid_geometry,
        "partition contains incomplete or zero SFS geometry",
        result.index,
        *start)};
  }
  result.name = *name;
  result.cluster_count = *cluster_count;
  result.sectors_per_cluster = *sectors_per_cluster;
  result.bitmap_cluster = *bitmap_cluster;
  result.directory_index_cluster = *index_cluster;
  result.directory_index_span_clusters = *index_span;
  std::copy_n(header->begin() + 0xac, result.unresolved_header_tail.size(),
              result.unresolved_header_tail.begin());

  const auto cluster_bytes = checked_multiply(sector_size, result.sectors_per_cluster);
  const auto raw_index_bytes = cluster_bytes
                                   ? checked_multiply(*cluster_bytes, result.directory_index_span_clusters)
                                   : Result<std::uint64_t>{std::unexpected{cluster_bytes.error()}};
  if (!cluster_bytes || !raw_index_bytes || *raw_index_bytes > options.max_index_bytes ||
      *raw_index_bytes > std::numeric_limits<std::size_t>::max()) {
    return std::unexpected{partition_error(
        ErrorCode::container_invalid_geometry,
        "partition index span exceeds configured bounds",
        result.index,
        *start + 0xa8U)};
  }
  const auto index_offset = cluster_offset(
      result.start_sector,
      sector_size,
      result.sectors_per_cluster,
      result.directory_index_cluster);
  if (!index_offset) {
    return std::unexpected{index_offset.error()};
  }
  const auto index_data = read_bytes(
      image, *index_offset, static_cast<std::size_t>(*raw_index_bytes), options.cancellation);
  if (!index_data) {
    return std::unexpected{index_data.error()};
  }

  std::vector<std::byte> reconstructed((result.cluster_count + 7U) / 8U);
  for (std::size_t block = 0; block + index_block_size <= index_data->size();
       block += index_block_size) {
    if (const auto check = options.cancellation.check(); !check) {
      return std::unexpected{check.error()};
    }
    for (std::size_t slot = 0; slot < records_per_index_block; ++slot) {
      const auto relative = block + slot * index_record_size;
      const auto bytes = std::span<const std::byte>{*index_data}.subspan(
          relative, static_cast<std::size_t>(index_record_size));
      if (std::all_of(bytes.begin(), bytes.begin() + 4, [](std::byte value) {
            return value == std::byte{};
          })) {
        continue;
      }
      const auto parsed = parse_record_header(bytes);
      if (!parsed) {
        result.diagnostics.push_back(partition_error(
            ErrorCode::object_malformed,
            "nonempty SFS index slot has an invalid record header",
            result.index,
            *index_offset + relative));
        continue;
      }
      IndexRecord record;
      record.sfs_id = SfsId{static_cast<std::uint32_t>(
          (block / index_block_size) * records_per_index_block + slot)};
      record.record_offset = ByteOffset{*index_offset + relative};
      record.extent_count = parsed->extent_count;
      record.cluster_count = parsed->cluster_count;
      record.data_size = parsed->data_size;
      Result<std::vector<Extent>> extents = std::unexpected{make_error(
          ErrorCode::allocation_invalid_extent,
          ErrorCategory::allocation,
          "extent parser was not selected")};
      if (record.extent_count <= direct_extent_limit) {
        extents = direct_extents(bytes, record.extent_count);
      } else {
        const ByteReader record_reader{bytes};
        const auto list_cluster = record_reader.be32(0x0a);
        if (list_cluster) {
          extents = continuation_extents(
              image,
              result,
              sector_size,
              *list_cluster,
              record.extent_count,
              record.continuation_clusters,
              options);
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
      for (const auto list_cluster : record.continuation_clusters) {
        if (list_cluster < result.cluster_count) {
          bitmap_set(reconstructed, list_cluster);
        }
      }
      for (const auto& extent : record.extents) {
        extent_cluster_sum += extent.cluster_count;
        if (extent.cluster_offset >= result.cluster_count ||
            extent.cluster_count > result.cluster_count - extent.cluster_offset) {
          ++result.allocation.invalid_extent_record_count;
          result.diagnostics.push_back(partition_error(
              ErrorCode::allocation_invalid_extent,
              "SFS extent lies outside the partition cluster range",
              result.index,
              record.record_offset.value));
          continue;
        }
        for (std::uint32_t cluster = extent.cluster_offset;
             cluster < extent.cluster_offset + extent.cluster_count;
             ++cluster) {
          bitmap_set(reconstructed, cluster);
        }
      }
      if (extent_cluster_sum != record.cluster_count) {
        ++result.allocation.extent_total_mismatch_count;
      }
      auto prefix = read_logical_prefix(
          image,
          result,
          sector_size,
          record,
          0x200U,
          options);
      if (prefix) {
        classify_record(record, *prefix);
        if (record.payload_kind == PayloadKind::directory &&
            record.data_size > prefix->size() &&
            record.data_size <= options.max_directory_bytes) {
          const auto expanded = read_logical_prefix(
              image,
              result,
              sector_size,
              record,
              static_cast<std::size_t>(record.data_size),
              options);
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

  validate_directory_graph(result, options);

  const auto bitmap_bytes_u64 = (static_cast<std::uint64_t>(result.cluster_count) + 7U) / 8U;
  if (bitmap_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
    return std::unexpected{partition_error(
        ErrorCode::container_invalid_geometry,
        "partition bitmap exceeds the supported memory range",
        result.index)};
  }
  const auto bitmap_offset = cluster_offset(
      result.start_sector, sector_size, result.sectors_per_cluster, result.bitmap_cluster);
  if (!bitmap_offset) {
    return std::unexpected{bitmap_offset.error()};
  }
  const auto stored = read_bytes(
      image,
      *bitmap_offset,
      static_cast<std::size_t>(bitmap_bytes_u64),
      options.cancellation);
  if (!stored) {
    return std::unexpected{stored.error()};
  }
  result.allocation.stored_used_cluster_count = count_bitmap_bits(*stored, result.cluster_count);
  result.allocation.reconstructed_used_cluster_count =
      count_bitmap_bits(reconstructed, result.cluster_count);
  result.allocation.stored_not_reconstructed = mismatch_ranges(
      *stored, reconstructed, result.cluster_count, options.max_mismatch_ranges);
  result.allocation.reconstructed_not_stored = mismatch_ranges(
      reconstructed, *stored, result.cluster_count, options.max_mismatch_ranges);
  const auto first_payload_u64 = checked_add(
      result.directory_index_cluster, result.directory_index_span_clusters);
  if (first_payload_u64 && *first_payload_u64 <= std::numeric_limits<std::uint32_t>::max() &&
      *cluster_bytes <= std::numeric_limits<std::uint32_t>::max()) {
    const auto free = calculate_sfs_free_space(
        result.cluster_count,
        static_cast<std::uint32_t>(*first_payload_u64),
        result.allocation.stored_used_cluster_count,
        static_cast<std::uint32_t>(*cluster_bytes));
    if (free) {
      result.allocation.free_space = *free;
    } else {
      result.diagnostics.push_back(free.error());
    }
  }
  return result;
}

}  // namespace

const std::filesystem::path& Container::source_path() const noexcept { return source_path_; }
std::uint64_t Container::image_size_bytes() const noexcept { return reader_->size(); }
const Superblock& Container::superblock() const noexcept { return superblock_; }
bool Container::backup_superblock_matches() const noexcept {
  return backup_superblock_matches_;
}
const std::vector<Partition>& Container::partitions() const noexcept { return partitions_; }
const std::vector<Error>& Container::diagnostics() const noexcept { return diagnostics_; }

Result<std::vector<std::byte>> Container::read_record_data(
    PartitionIndex partition_index,
    SfsId record_id,
    std::size_t maximum_bytes,
    const CancellationToken& cancellation) const {
  const auto partition = std::find_if(
      partitions_.begin(), partitions_.end(), [&](const Partition& item) {
        return item.index == partition_index;
      });
  if (partition == partitions_.end()) {
    ErrorContext context;
    context.partition_index = partition_index.value;
    return std::unexpected{make_error(
        ErrorCode::object_missing,
        ErrorCategory::object,
        "partition is not available in the open image",
        std::move(context))};
  }
  const auto record = std::find_if(
      partition->records.begin(), partition->records.end(), [&](const IndexRecord& item) {
        return item.sfs_id == record_id;
      });
  if (record == partition->records.end()) {
    ErrorContext context;
    context.partition_index = partition_index.value;
    context.object_name = "SFS ID " + std::to_string(record_id.value);
    return std::unexpected{make_error(
        ErrorCode::object_missing,
        ErrorCategory::object,
        "SFS record is not available in the partition",
        std::move(context))};
  }
  if (record->data_size > maximum_bytes) {
    ErrorContext context;
    context.partition_index = partition_index.value;
    context.object_type = record->object_type;
    context.object_name = record->object_name;
    context.raw_offset = record->record_offset.value;
    return std::unexpected{make_error(
        ErrorCode::out_of_bounds,
        ErrorCategory::object,
        "object payload exceeds the caller's read limit",
        std::move(context))};
  }
  OpenOptions options;
  options.cancellation = cancellation;
  const auto data = read_logical_prefix(
      *reader_,
      *partition,
      superblock_.sector_size_bytes,
      *record,
      record->data_size,
      options);
  if (!data) {
    auto error = data.error();
    error.context.partition_index = partition_index.value;
    error.context.object_type = record->object_type;
    error.context.object_name = record->object_name;
    error.context.raw_offset = record->record_offset.value;
    return std::unexpected{std::move(error)};
  }
  if (data->size() != record->data_size) {
    return std::unexpected{make_error(
        ErrorCode::io_short_read,
        ErrorCategory::io,
        "logical SFS record read did not produce its declared size")};
  }
  return std::move(*data);
}

Result<SfsFreeSpace> calculate_sfs_free_space(
    std::uint32_t cluster_count,
    std::uint32_t first_payload_cluster,
    std::uint32_t allocated_cluster_count,
    std::uint32_t cluster_size_bytes) {
  if (first_payload_cluster > cluster_count || cluster_size_bytes == 0) {
    return std::unexpected{make_error(
        ErrorCode::container_invalid_geometry,
        ErrorCategory::allocation,
        "free-space geometry has an invalid reserved prefix or cluster size")};
  }
  const auto available = cluster_count - first_payload_cluster;
  if (allocated_cluster_count > available) {
    return std::unexpected{make_error(
        ErrorCode::allocation_mismatch,
        ErrorCategory::allocation,
        "allocated clusters exceed the payload cluster range")};
  }
  const auto free_clusters = available - allocated_cluster_count;
  const auto free_bytes = checked_multiply(free_clusters, cluster_size_bytes);
  if (!free_bytes) {
    return std::unexpected{free_bytes.error()};
  }
  return SfsFreeSpace{
      cluster_count,
      first_payload_cluster,
      allocated_cluster_count,
      free_clusters,
      cluster_size_bytes,
      *free_bytes,
      *free_bytes / 1024U};
}

Result<Container> open_image(const std::filesystem::path& path, const OpenOptions& options) {
  const auto reader = FileReader::open(path);
  if (!reader) {
    return std::unexpected{reader.error()};
  }
  return open_image(*reader, path, options);
}

Result<Container> open_image(
    std::shared_ptr<const RandomAccessReader> image,
    std::filesystem::path source_path,
    const OpenOptions& options) {
  if (!image) {
    return std::unexpected{make_error(
        ErrorCode::invalid_argument,
        ErrorCategory::io,
        "image reader must not be null")};
  }
  if (options.progress) {
    options.progress->report(
        {ProgressPhase::opening, 0, std::nullopt, text::path_to_utf8(source_path), {}});
  }
  const auto primary_bytes = read_bytes(
      *image, 0, sfs_default_sector_size, options.cancellation);
  if (!primary_bytes) {
    return std::unexpected{primary_bytes.error()};
  }
  const auto primary = parse_superblock(*primary_bytes);
  if (!primary) {
    auto error = primary.error();
    error.context.source_path = text::path_to_utf8(source_path);
    return std::unexpected{std::move(error)};
  }
  if (primary->sector_size_bytes == 0 || primary->sector_size_bytes > 65536U) {
    return std::unexpected{make_error(
        ErrorCode::container_invalid_geometry,
        ErrorCategory::container,
        "SFS sector size is outside the supported range")};
  }
  const auto backup_bytes = read_bytes(
      *image, primary->sector_size_bytes, sfs_default_sector_size, options.cancellation);
  if (!backup_bytes) {
    return std::unexpected{backup_bytes.error()};
  }
  Container result;
  result.source_path_ = std::move(source_path);
  result.reader_ = std::move(image);
  result.superblock_ = *primary;
  result.backup_superblock_matches_ = *primary_bytes == *backup_bytes;
  if (!result.backup_superblock_matches_) {
    ErrorContext context;
    context.source_path = text::path_to_utf8(result.source_path_);
    context.raw_offset = primary->sector_size_bytes;
    result.diagnostics_.push_back(make_error(
        ErrorCode::container_backup_mismatch,
        ErrorCategory::container,
        "backup superblock differs from primary",
        std::move(context)));
  }
  for (const auto& entry : primary->partition_entries) {
    if (!entry.active()) {
      continue;
    }
    const auto end_sector = checked_add(entry.start_sector, entry.sector_count);
    const auto end_offset = end_sector
                                ? checked_multiply(*end_sector, primary->sector_size_bytes)
                                : Result<std::uint64_t>{std::unexpected{end_sector.error()}};
    if (!end_sector || !end_offset || *end_offset > result.reader_->size()) {
      auto error = partition_error(
          ErrorCode::container_partition_out_of_range,
          "partition extends beyond the input image",
          entry.index);
      error.context.source_path = text::path_to_utf8(result.source_path_);
      result.diagnostics_.push_back(std::move(error));
      continue;
    }
    const auto partition = parse_partition(
        *result.reader_, entry, primary->sector_size_bytes, options);
    if (!partition) {
      auto error = partition.error();
      error.context.source_path = text::path_to_utf8(result.source_path_);
      error.context.partition_index = entry.index.value;
      result.diagnostics_.push_back(std::move(error));
      continue;
    }
    result.partitions_.push_back(*partition);
    if (options.progress) {
      options.progress->report({
          ProgressPhase::reading,
          result.partitions_.size(),
          std::nullopt,
          result.partitions_.back().name,
          {}});
    }
  }
  return result;
}

}  // namespace axk
