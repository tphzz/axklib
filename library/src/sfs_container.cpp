#include "axklib/sfs.hpp"

#include "axklib/bytes.hpp"
#include "axklib/utf8.hpp"
#include "sfs_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace axk {

const std::filesystem::path &Container::source_path() const noexcept { return source_path_; }
std::uint64_t Container::image_size_bytes() const noexcept { return reader_->size(); }
const Superblock &Container::superblock() const noexcept { return superblock_; }
bool Container::backup_superblock_matches() const noexcept { return backup_superblock_matches_; }
const std::vector<Partition> &Container::partitions() const noexcept { return partitions_; }
const std::vector<Error> &Container::diagnostics() const noexcept { return diagnostics_; }

Result<std::vector<std::byte>> Container::read_record_data(PartitionIndex partition_index, SfsId record_id,
                                                           std::size_t maximum_bytes,
                                                           const CancellationToken &cancellation) const {
    const auto partition = std::find_if(partitions_.begin(), partitions_.end(),
                                        [&](const Partition &item) { return item.index == partition_index; });
    if (partition == partitions_.end()) {
        ErrorContext context;
        context.partition_index = partition_index.value;
        return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                          "partition is not available in the open image", std::move(context))};
    }
    const auto record = std::find_if(partition->records.begin(), partition->records.end(),
                                     [&](const IndexRecord &item) { return item.sfs_id == record_id; });
    if (record == partition->records.end()) {
        ErrorContext context;
        context.partition_index = partition_index.value;
        context.object_name = "SFS ID " + std::to_string(record_id.value);
        return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                          "SFS record is not available in the partition", std::move(context))};
    }
    if (record->data_size > maximum_bytes) {
        ErrorContext context;
        context.partition_index = partition_index.value;
        context.object_type = record->object_type;
        context.object_name = record->object_name;
        context.raw_offset = record->record_offset.value;
        return std::unexpected{make_error(ErrorCode::out_of_bounds, ErrorCategory::object,
                                          "object payload exceeds the caller's read limit", std::move(context))};
    }
    OpenOptions options;
    options.cancellation = cancellation;
    const auto data = sfs_detail::read_logical_prefix(*reader_, *partition, superblock_.sector_size_bytes, *record,
                                                      record->data_size, options);
    if (!data) {
        auto error = data.error();
        error.context.partition_index = partition_index.value;
        error.context.object_type = record->object_type;
        error.context.object_name = record->object_name;
        error.context.raw_offset = record->record_offset.value;
        return std::unexpected{std::move(error)};
    }
    if (data->size() != record->data_size) {
        return std::unexpected{make_error(ErrorCode::io_short_read, ErrorCategory::io,
                                          "logical SFS record read did not produce its declared size")};
    }
    return std::move(*data);
}

Result<std::vector<std::byte>> Container::read_record_range(PartitionIndex partition_index, SfsId record_id,
                                                            std::uint64_t offset, std::size_t size,
                                                            const CancellationToken &cancellation) const {
    const auto partition = std::find_if(partitions_.begin(), partitions_.end(),
                                        [&](const Partition &item) { return item.index == partition_index; });
    if (partition == partitions_.end()) {
        return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                          "partition is not available in the open image")};
    }
    const auto record = std::find_if(partition->records.begin(), partition->records.end(),
                                     [&](const IndexRecord &item) { return item.sfs_id == record_id; });
    if (record == partition->records.end()) {
        return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                          "SFS record is not available in the partition")};
    }
    OpenOptions options;
    options.cancellation = cancellation;
    auto result = sfs_detail::read_logical_range(*reader_, *partition, superblock_.sector_size_bytes, *record, offset,
                                                 size, options);
    if (!result) {
        auto error = result.error();
        error.context.partition_index = partition_index.value;
        error.context.object_type = record->object_type;
        error.context.object_name = record->object_name;
        error.context.raw_offset = record->record_offset.value;
        return std::unexpected{std::move(error)};
    }
    return result;
}

Result<SfsFreeSpace> calculate_sfs_free_space(std::uint32_t cluster_count, std::uint32_t first_payload_cluster,
                                              std::uint32_t allocated_cluster_count, std::uint32_t cluster_size_bytes) {
    if (first_payload_cluster > cluster_count || cluster_size_bytes == 0) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::allocation,
                                          "free-space geometry has an invalid "
                                          "reserved prefix or cluster size")};
    }
    const auto available = cluster_count - first_payload_cluster;
    if (allocated_cluster_count > available) {
        return std::unexpected{make_error(ErrorCode::allocation_mismatch, ErrorCategory::allocation,
                                          "allocated clusters exceed the payload cluster range")};
    }
    const auto free_clusters = available - allocated_cluster_count;
    const auto free_bytes = checked_multiply(free_clusters, cluster_size_bytes);
    if (!free_bytes)
        return std::unexpected{free_bytes.error()};
    return SfsFreeSpace{cluster_count,      first_payload_cluster, allocated_cluster_count, free_clusters,
                        cluster_size_bytes, *free_bytes,           *free_bytes / 1024U};
}

Result<Container> open_image(const std::filesystem::path &path, const OpenOptions &options) {
    const auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return open_image(*reader, path, options);
}

Result<Container> open_image(std::shared_ptr<const RandomAccessReader> image, std::filesystem::path source_path,
                             const OpenOptions &options) {
    if (!image) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "image reader must not be null")};
    }
    if (options.progress)
        options.progress->report({ProgressPhase::opening, 0, std::nullopt, text::path_to_utf8(source_path), {}});
    const auto primary_bytes = sfs_detail::read_bytes(*image, 0, sfs_default_sector_size, options.cancellation);
    if (!primary_bytes)
        return std::unexpected{primary_bytes.error()};
    const auto primary = sfs_detail::parse_superblock(*primary_bytes);
    if (!primary) {
        auto error = primary.error();
        error.context.source_path = text::path_to_utf8(source_path);
        return std::unexpected{std::move(error)};
    }
    if (primary->sector_size_bytes == 0 || primary->sector_size_bytes > 65536U) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "SFS sector size is outside the supported range")};
    }
    const auto backup_bytes =
        sfs_detail::read_bytes(*image, primary->sector_size_bytes, sfs_default_sector_size, options.cancellation);
    if (!backup_bytes)
        return std::unexpected{backup_bytes.error()};
    Container result;
    result.source_path_ = std::move(source_path);
    result.reader_ = std::move(image);
    result.superblock_ = *primary;
    result.backup_superblock_matches_ = *primary_bytes == *backup_bytes;
    if (!result.backup_superblock_matches_) {
        ErrorContext context;
        context.source_path = text::path_to_utf8(result.source_path_);
        context.raw_offset = primary->sector_size_bytes;
        result.diagnostics_.push_back(make_error(ErrorCode::container_backup_mismatch, ErrorCategory::container,
                                                 "backup superblock differs from primary", std::move(context)));
    }
    for (const auto &entry : primary->partition_entries) {
        if (!entry.active())
            continue;
        const auto end_sector = checked_add(entry.start_sector, entry.sector_count);
        const auto end_offset = end_sector ? checked_multiply(*end_sector, primary->sector_size_bytes)
                                           : Result<std::uint64_t>{std::unexpected{end_sector.error()}};
        if (!end_sector || !end_offset || *end_offset > result.reader_->size()) {
            auto error = sfs_detail::partition_error(ErrorCode::container_partition_out_of_range,
                                                     "partition extends beyond the input image", entry.index);
            error.context.source_path = text::path_to_utf8(result.source_path_);
            result.diagnostics_.push_back(std::move(error));
            continue;
        }
        const auto partition = sfs_detail::parse_partition(*result.reader_, entry, primary->sector_size_bytes, options);
        if (!partition) {
            auto error = partition.error();
            error.context.source_path = text::path_to_utf8(result.source_path_);
            error.context.partition_index = entry.index.value;
            result.diagnostics_.push_back(std::move(error));
            continue;
        }
        result.partitions_.push_back(*partition);
        if (options.progress) {
            options.progress->report(
                {ProgressPhase::reading, result.partitions_.size(), std::nullopt, result.partitions_.back().name, {}});
        }
    }
    return result;
}

} // namespace axk
