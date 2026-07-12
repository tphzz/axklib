#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"
#include "axklib/types.hpp"

namespace axk {

inline constexpr std::uint32_t sfs_default_sector_size = 512;
inline constexpr std::size_t sfs_partition_limit = 8;

struct OpenOptions {
  std::size_t max_index_bytes{4U * 1024U * 1024U};
  std::size_t max_directory_bytes{64U * 1024U};
  std::size_t max_mismatch_ranges{512};
  CancellationToken cancellation;
  ProgressSink* progress{};
};

struct PartitionEntry {
  PartitionIndex index;
  std::uint32_t start_sector{};
  std::uint32_t sector_count{};

  [[nodiscard]] bool active() const noexcept {
    return start_sector != 0 || sector_count != 0;
  }
};

struct Superblock {
  std::uint32_t sector_size_bytes{};
  std::uint32_t total_sector_count{};
  std::array<std::byte, 0x1c> unresolved_formatter_words{};
  std::array<PartitionEntry, sfs_partition_limit> partition_entries{};
};

struct Extent {
  std::uint32_t cluster_offset{};
  std::uint32_t cluster_count{};
  std::uint32_t byte_count{};
};

struct DirectoryEntry {
  std::uint16_t flags{};
  LinkId link_id;
  std::string name;
  std::uint64_t payload_relative_offset{};
};

enum class PayloadKind : std::uint8_t {
  unknown,
  directory,
  object,
  alternating_byte_object,
};

struct IndexRecord {
  SfsId sfs_id;
  ByteOffset record_offset;
  std::uint16_t extent_count{};
  std::uint16_t cluster_count{};
  std::uint32_t data_size{};
  std::vector<Extent> extents;
  std::vector<std::uint32_t> continuation_clusters;
  PayloadKind payload_kind{PayloadKind::unknown};
  std::string object_type;
  std::string object_name;
  std::optional<LinkId> directory_id;
  std::optional<LinkId> parent_directory_id;
  std::vector<DirectoryEntry> directory_entries;
};

struct AllocationMismatchRange {
  std::uint32_t start_cluster{};
  std::uint32_t end_cluster{};
};

struct SfsFreeSpace {
  std::uint32_t total_cluster_count{};
  std::uint32_t reserved_cluster_count{};
  std::uint32_t allocated_cluster_count{};
  std::uint32_t free_cluster_count{};
  std::uint32_t cluster_size_bytes{};
  std::uint64_t free_bytes{};
  std::uint64_t sampler_visible_free_kib{};
};

struct AllocationSummary {
  std::uint32_t stored_used_cluster_count{};
  std::uint32_t reconstructed_used_cluster_count{};
  std::uint32_t invalid_extent_record_count{};
  std::uint32_t extent_total_mismatch_count{};
  std::vector<AllocationMismatchRange> stored_not_reconstructed;
  std::vector<AllocationMismatchRange> reconstructed_not_stored;
  std::optional<SfsFreeSpace> free_space;
};

struct Partition {
  PartitionIndex index;
  std::string name;
  std::uint32_t start_sector{};
  std::uint32_t sector_count{};
  std::uint32_t cluster_count{};
  std::uint32_t sectors_per_cluster{};
  std::uint32_t bitmap_cluster{};
  std::uint32_t directory_index_cluster{};
  std::uint32_t directory_index_span_clusters{};
  bool backup_header_matches{};
  std::array<std::byte, 0x154> unresolved_header_tail{};
  std::vector<IndexRecord> records;
  AllocationSummary allocation;
  std::vector<Error> diagnostics;
};

class AXK_API Container {
 public:
  [[nodiscard]] const std::filesystem::path& source_path() const noexcept;
  [[nodiscard]] std::uint64_t image_size_bytes() const noexcept;
  [[nodiscard]] const Superblock& superblock() const noexcept;
  [[nodiscard]] bool backup_superblock_matches() const noexcept;
  [[nodiscard]] const std::vector<Partition>& partitions() const noexcept;
  [[nodiscard]] const std::vector<Error>& diagnostics() const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>> read_record_data(
      PartitionIndex partition,
      SfsId record,
      std::size_t maximum_bytes,
      const CancellationToken& cancellation = {}) const;

 private:
  std::filesystem::path source_path_;
  std::shared_ptr<const RandomAccessReader> reader_;
  Superblock superblock_;
  bool backup_superblock_matches_{};
  std::vector<Partition> partitions_;
  std::vector<Error> diagnostics_;

  friend AXK_API Result<Container> open_image(const std::filesystem::path&, const OpenOptions&);
  friend AXK_API Result<Container> open_image(
      std::shared_ptr<const RandomAccessReader>, std::filesystem::path, const OpenOptions&);
};

AXK_API Result<SfsFreeSpace> calculate_sfs_free_space(
    std::uint32_t cluster_count,
    std::uint32_t first_payload_cluster,
    std::uint32_t allocated_cluster_count,
    std::uint32_t cluster_size_bytes = 1024);

AXK_API Result<Container> open_image(
    const std::filesystem::path& path,
    const OpenOptions& options = {});
AXK_API Result<Container> open_image(
    std::shared_ptr<const RandomAccessReader> reader,
    std::filesystem::path source_path,
    const OpenOptions& options = {});

}  // namespace axk
