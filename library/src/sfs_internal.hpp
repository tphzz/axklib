#pragma once

#include "axklib/sfs.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace axk::sfs_detail {

Result<std::vector<std::byte>> read_bytes(const RandomAccessReader &reader, std::uint64_t offset, std::size_t count,
                                          const CancellationToken &cancellation);
bool begins_with(std::span<const std::byte> bytes, std::string_view value);
Error partition_error(ErrorCode code, std::string message, PartitionIndex index, std::uint64_t offset = 0);
Result<Superblock> parse_superblock(std::span<const std::byte> bytes);
Result<std::vector<std::byte>> read_logical_prefix(const RandomAccessReader &image, const Partition &partition,
                                                   std::uint32_t sector_size, const IndexRecord &record,
                                                   std::size_t limit, const OpenOptions &options);
Result<std::vector<std::byte>> read_logical_range(const RandomAccessReader &image, const Partition &partition,
                                                  std::uint32_t sector_size, const IndexRecord &record,
                                                  std::uint64_t requested_offset, std::size_t requested_size,
                                                  const OpenOptions &options);
std::vector<DirectoryEntry> parse_directory_entries(std::span<const std::byte> payload);
void classify_record(IndexRecord &record, std::span<const std::byte> payload);
Result<void> validate_directory_graph(Partition &partition, const OpenOptions &options);
Result<Partition> parse_partition(const RandomAccessReader &image, const PartitionEntry &table_entry,
                                  std::uint32_t sector_size, const OpenOptions &options);

} // namespace axk::sfs_detail
