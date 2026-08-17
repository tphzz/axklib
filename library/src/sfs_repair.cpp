#include "axklib/sfs_repair.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/sfs_allocation.hpp"

namespace axk {
namespace {

constexpr std::size_t direct_extent_limit = 4U;
constexpr std::size_t index_record_size = 72U;
constexpr std::size_t continuation_header_size = 12U;

Error repair_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Result<void> require_distinct_paths(const std::filesystem::path &source, const std::filesystem::path &output) {
    std::error_code error;
    const auto canonical_source = std::filesystem::canonical(source, error);
    if (error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify extent-layout repair source")};
    const auto canonical_output = std::filesystem::weakly_canonical(output, error);
    if (error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify extent-layout repair output")};
    if (canonical_source == canonical_output)
        return std::unexpected{repair_error("extent-layout repair output must differ from source")};
    if (std::filesystem::exists(output, error) && std::filesystem::equivalent(source, output, error))
        return std::unexpected{repair_error("extent-layout repair output must differ from source")};
    if (error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not compare extent-layout repair paths")};
    return {};
}

Result<void> bitmap_write(std::span<std::byte> bitmap, std::uint32_t cluster, bool used) {
    const auto index = static_cast<std::size_t>(cluster / 8U);
    if (index >= bitmap.size())
        return std::unexpected{repair_error("extent-layout repair bitmap access is outside its range")};
    const auto mask = static_cast<std::byte>(0x80U >> (cluster & 7U));
    if (used)
        bitmap[index] |= mask;
    else
        bitmap[index] &= ~mask;
    return {};
}

Result<detail::TemporaryPublication> copy_source(const RandomAccessReader &source, const std::filesystem::path &output,
                                                 const CancellationToken &cancellation, ProgressSink *progress) {
    auto publication = detail::TemporaryPublication::create(output);
    if (!publication)
        return std::unexpected{publication.error()};
    if (auto resized = publication->resize(source.size()); !resized)
        return std::unexpected{resized.error()};
    constexpr std::size_t chunk_size = 1024U * 1024U;
    std::vector<std::byte> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), chunk_size)));
    for (std::uint64_t offset = 0U; offset < source.size(); offset += buffer.size()) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - offset));
        auto bytes = std::span{buffer}.first(count);
        if (auto read = source.read_exact_at(offset, bytes); !read)
            return std::unexpected{read.error()};
        if (auto written = publication->write_at(offset, bytes); !written)
            return std::unexpected{written.error()};
        if (progress)
            progress->report(
                {ProgressPhase::writing, offset + count, source.size(), "copying source image", output.string()});
    }
    return std::move(*publication);
}

bool allocation_has_only_repairable_byte_mismatches(const AllocationSummary &allocation) {
    return allocation.extent_byte_total_mismatch_count != 0U && allocation.stored_copies_match &&
           allocation.invalid_extent_record_count == 0U && allocation.extent_total_mismatch_count == 0U &&
           allocation.conflicting_cluster_count == 0U &&
           allocation.fixed_location.marked_used_without_index_extent_count == 0U &&
           allocation.fixed_location.index_extent_marked_free_count == 0U &&
           allocation.header_addressed.marked_used_without_index_extent_count == 0U &&
           allocation.header_addressed.index_extent_marked_free_count == 0U;
}

Result<SfsExtentLayoutRepairTarget> normalized_target(const Partition &partition, const IndexRecord &record) {
    if (record.extents.empty())
        return std::unexpected{repair_error("mismatching record has no data extents")};
    const auto cluster_bytes = checked_multiply(partition.sectors_per_cluster, sfs_default_sector_size);
    if (!cluster_bytes)
        return std::unexpected{repair_error("extent-layout repair payload geometry overflowed")};

    SfsExtentLayoutRepairTarget target;
    target.partition = partition.index;
    target.record = record.sfs_id;
    target.source_extents = record.extents;
    target.source_continuation_clusters = record.continuation_clusters;
    target.logical_size = record.data_size;

    std::uint64_t remaining = record.data_size;
    if (record.extent_byte_count_total < record.data_size) {
        if (record.extents.size() != 1U)
            return std::unexpected{
                repair_error("underreported multi-extent layout is ambiguous and cannot be repaired")};
        const auto capacity = checked_multiply(record.extents.front().cluster_count, *cluster_bytes);
        if (!capacity || *capacity < record.data_size)
            return std::unexpected{repair_error("allocated extent cannot hold the complete logical record payload")};
        target.replacement_extents.push_back(
            {record.extents.front().cluster_offset, record.extents.front().cluster_count, record.data_size});
        remaining = 0U;
    } else {
        for (const auto &extent : record.extents) {
            const auto capacity = checked_multiply(extent.cluster_count, *cluster_bytes);
            if (!capacity || extent.byte_count > *capacity)
                return std::unexpected{repair_error("mismatching extent exceeds its allocated cluster capacity")};
            if (remaining == 0U)
                continue;
            const auto byte_count = std::min<std::uint64_t>(remaining, extent.byte_count);
            if (byte_count == 0U || byte_count > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected{repair_error("normalized extent byte count is outside the supported range")};
            target.replacement_extents.push_back(
                {extent.cluster_offset, extent.cluster_count, static_cast<std::uint32_t>(byte_count)});
            remaining -= byte_count;
        }
    }
    if (remaining != 0U)
        return std::unexpected{repair_error("extent layout cannot supply the complete logical record payload")};

    if (target.replacement_extents.size() > direct_extent_limit) {
        if (*cluster_bytes <= continuation_header_size)
            return std::unexpected{repair_error("SFS continuation-list cluster is too small")};
        const auto extents_per_cluster = (*cluster_bytes - continuation_header_size) / 12U;
        const auto needed = (target.replacement_extents.size() + extents_per_cluster - 1U) / extents_per_cluster;
        if (needed > target.source_continuation_clusters.size())
            return std::unexpected{repair_error("extent layout lacks required continuation-list clusters")};
        target.replacement_continuation_clusters.assign(target.source_continuation_clusters.begin(),
                                                        target.source_continuation_clusters.begin() +
                                                            static_cast<std::ptrdiff_t>(needed));
    }
    return target;
}

Result<std::vector<std::byte>> read_source_logical_payload(const RandomAccessReader &source, const Partition &partition,
                                                           const IndexRecord &record,
                                                           const CancellationToken &cancellation) {
    const auto cluster_bytes = checked_multiply(partition.sectors_per_cluster, sfs_default_sector_size);
    const auto partition_offset = checked_multiply(partition.start_sector, sfs_default_sector_size);
    if (!cluster_bytes || !partition_offset)
        return std::unexpected{repair_error("extent-layout repair payload geometry overflowed")};

    std::vector<std::byte> payload(record.data_size);
    std::size_t payload_offset{};
    for (const auto &extent : record.extents) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto capacity = checked_multiply(extent.cluster_count, *cluster_bytes);
        const auto relative_offset = checked_multiply(extent.cluster_offset, *cluster_bytes);
        if (!capacity || !relative_offset)
            return std::unexpected{repair_error("extent-layout repair payload geometry overflowed")};
        const auto absolute_offset = checked_add(*partition_offset, *relative_offset);
        if (!absolute_offset)
            return std::unexpected{repair_error("extent-layout repair payload geometry overflowed")};
        const auto available = record.extent_byte_count_total < record.data_size ? *capacity : extent.byte_count;
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(available, static_cast<std::uint64_t>(payload.size() - payload_offset)));
        if (auto read = source.read_exact_at(*absolute_offset, std::span{payload}.subspan(payload_offset, count));
            !read)
            return std::unexpected{read.error()};
        payload_offset += count;
        if (payload_offset == payload.size())
            break;
    }
    if (payload_offset != payload.size())
        return std::unexpected{repair_error("allocated extents cannot supply the complete logical record payload")};
    return payload;
}

Result<std::uint16_t> replacement_cluster_count(const SfsExtentLayoutRepairTarget &target) {
    std::uint64_t total{};
    for (const auto &extent : target.replacement_extents)
        total += extent.cluster_count;
    if (total == 0U || total > std::numeric_limits<std::uint16_t>::max())
        return std::unexpected{repair_error("normalized extent cluster total is outside the SFS record range")};
    return static_cast<std::uint16_t>(total);
}

Result<std::array<std::byte, index_record_size>> normalized_index(const RandomAccessReader &source,
                                                                  const IndexRecord &record,
                                                                  const SfsExtentLayoutRepairTarget &target) {
    std::array<std::byte, index_record_size> index{};
    if (auto read = source.read_exact_at(record.record_offset.value, index); !read)
        return std::unexpected{read.error()};
    std::fill(index.begin() + 0x0a, index.begin() + 0x3a, std::byte{});
    ByteWriter writer{index};
    const auto cluster_count = replacement_cluster_count(target);
    if (!cluster_count)
        return std::unexpected{cluster_count.error()};
    if (target.replacement_extents.size() > std::numeric_limits<std::uint16_t>::max())
        return std::unexpected{repair_error("normalized extent count is outside the SFS record range")};
    if (auto written = writer.write_be16(0U, static_cast<std::uint16_t>(target.replacement_extents.size())); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be16(4U, *cluster_count); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(6U, static_cast<std::uint32_t>(target.logical_size)); !written)
        return std::unexpected{written.error()};
    if (target.replacement_extents.size() <= direct_extent_limit) {
        for (std::size_t extent_index = 0; extent_index < target.replacement_extents.size(); ++extent_index) {
            const auto &extent = target.replacement_extents[extent_index];
            const auto offset = 0x0aU + extent_index * 12U;
            if (auto written = writer.write_be32(offset, extent.cluster_offset); !written)
                return std::unexpected{written.error()};
            if (auto written = writer.write_be32(offset + 4U, extent.cluster_count); !written)
                return std::unexpected{written.error()};
            if (auto written = writer.write_be32(offset + 8U, extent.byte_count); !written)
                return std::unexpected{written.error()};
        }
    } else {
        if (target.replacement_continuation_clusters.empty())
            return std::unexpected{repair_error("normalized record requires a continuation list")};
        if (auto written = writer.write_be32(0x0aU, target.replacement_continuation_clusters.front()); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x0eU, *cluster_count); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x12U, static_cast<std::uint32_t>(target.logical_size)); !written)
            return std::unexpected{written.error()};
    }
    return index;
}

Result<void> write_continuation_lists(detail::TemporaryPublication &publication, const Partition &partition,
                                      const SfsExtentLayoutRepairTarget &target) {
    if (target.replacement_continuation_clusters.empty())
        return {};
    const auto cluster_bytes_u64 = checked_multiply(partition.sectors_per_cluster, sfs_default_sector_size);
    if (!cluster_bytes_u64 || *cluster_bytes_u64 > std::numeric_limits<std::size_t>::max() ||
        *cluster_bytes_u64 <= continuation_header_size)
        return std::unexpected{repair_error("SFS continuation-list geometry is outside the supported range")};
    const auto cluster_bytes = static_cast<std::size_t>(*cluster_bytes_u64);
    const auto extents_per_cluster = (cluster_bytes - continuation_header_size) / 12U;
    for (std::size_t list_index = 0; list_index < target.replacement_continuation_clusters.size(); ++list_index) {
        const auto extent_begin = list_index * extents_per_cluster;
        const auto extent_count = std::min(extents_per_cluster, target.replacement_extents.size() - extent_begin);
        std::vector<std::byte> block(cluster_bytes);
        ByteWriter writer{block};
        if (auto written = writer.write_be32(0U, static_cast<std::uint32_t>(extent_count)); !written)
            return std::unexpected{written.error()};
        const auto next = list_index + 1U < target.replacement_continuation_clusters.size()
                              ? target.replacement_continuation_clusters[list_index + 1U]
                              : 0U;
        if (auto written = writer.write_be32(8U, next); !written)
            return std::unexpected{written.error()};
        for (std::size_t index = 0; index < extent_count; ++index) {
            const auto &extent = target.replacement_extents[extent_begin + index];
            const auto offset = continuation_header_size + index * 12U;
            if (auto written = writer.write_be32(offset, extent.cluster_offset); !written)
                return std::unexpected{written.error()};
            if (auto written = writer.write_be32(offset + 4U, extent.cluster_count); !written)
                return std::unexpected{written.error()};
            if (auto written = writer.write_be32(offset + 8U, extent.byte_count); !written)
                return std::unexpected{written.error()};
        }
        const auto relative =
            checked_multiply(target.replacement_continuation_clusters[list_index], *cluster_bytes_u64);
        const auto partition_offset = checked_multiply(partition.start_sector, sfs_default_sector_size);
        if (!relative || !partition_offset)
            return std::unexpected{repair_error("SFS continuation-list offset overflowed")};
        const auto absolute = checked_add(*partition_offset, *relative);
        if (!absolute)
            return std::unexpected{repair_error("SFS continuation-list offset overflowed")};
        if (auto written = publication.write_at(*absolute, block); !written)
            return std::unexpected{written.error()};
    }
    return {};
}

Result<void> release_trailing_allocations(std::span<std::byte> bitmap, const SfsExtentLayoutRepairTarget &target) {
    for (std::size_t index = target.replacement_extents.size(); index < target.source_extents.size(); ++index) {
        const auto &extent = target.source_extents[index];
        for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
             ++cluster) {
            if (auto changed = bitmap_write(bitmap, cluster, false); !changed)
                return changed;
        }
    }
    for (std::size_t index = target.replacement_continuation_clusters.size();
         index < target.source_continuation_clusters.size(); ++index) {
        if (auto changed = bitmap_write(bitmap, target.source_continuation_clusters[index], false); !changed)
            return changed;
    }
    return {};
}

struct PreservedPayload {
    PartitionIndex partition;
    SfsId record;
    std::vector<std::byte> bytes;
};

} // namespace

Result<SfsExtentLayoutRepairPlan> inspect_sfs_extent_layout_repair(const Container &container) {
    SfsExtentLayoutRepairPlan plan;
    for (const auto &partition : container.partitions()) {
        if (partition.allocation.extent_byte_total_mismatch_count == 0U) {
            if (!allocation_is_safe_for_mutation(partition.allocation))
                return std::unexpected{
                    repair_error("extent-layout repair requires clean allocation metadata in every other partition")};
            continue;
        }
        if (!allocation_has_only_repairable_byte_mismatches(partition.allocation))
            return std::unexpected{repair_error("extent byte totals are not the only allocation inconsistency")};
        const auto initial_size = plan.targets.size();
        for (const auto &record : partition.records) {
            if (record.extent_byte_count_total == record.data_size)
                continue;
            auto target = normalized_target(partition, record);
            if (!target)
                return std::unexpected{target.error()};
            plan.targets.push_back(std::move(*target));
        }
        if (plan.targets.size() - initial_size != partition.allocation.extent_byte_total_mismatch_count)
            return std::unexpected{repair_error("not every reported extent byte-total mismatch has a repair target")};
    }
    if (plan.targets.empty())
        return std::unexpected{repair_error("image has no repairable SFS extent byte-total mismatch")};
    return plan;
}

Result<SfsExtentLayoutRepairResult> repair_sfs_extent_layout(const std::filesystem::path &source_path,
                                                             const std::filesystem::path &output_path,
                                                             const CancellationToken &cancellation,
                                                             ProgressSink *progress, bool overwrite) {
    if (auto distinct = require_distinct_paths(source_path, output_path); !distinct)
        return std::unexpected{distinct.error()};
    if (progress)
        progress->report(
            {ProgressPhase::opening, 0U, std::nullopt, "opening extent-layout repair source", output_path.string()});
    auto source = FileReader::open(source_path);
    if (!source)
        return std::unexpected{source.error()};
    OpenOptions options;
    options.cancellation = cancellation;
    auto container = open_image(*source, source_path, options);
    if (!container)
        return std::unexpected{container.error()};
    auto plan = inspect_sfs_extent_layout_repair(*container);
    if (!plan)
        return std::unexpected{plan.error()};

    std::vector<PreservedPayload> payloads;
    payloads.reserve(plan->targets.size());
    for (const auto &target : plan->targets) {
        const auto partition = std::ranges::find(container->partitions(), target.partition, &Partition::index);
        if (partition == container->partitions().end())
            return std::unexpected{repair_error("repair target partition disappeared")};
        const auto record = std::ranges::find(partition->records, target.record, &IndexRecord::sfs_id);
        if (record == partition->records.end())
            return std::unexpected{repair_error("repair target record disappeared")};
        auto payload = read_source_logical_payload(**source, *partition, *record, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        payloads.push_back({target.partition, target.record, std::move(*payload)});
    }

    auto publication = copy_source(**source, output_path, cancellation, progress);
    if (!publication)
        return std::unexpected{publication.error()};
    for (const auto &partition : container->partitions()) {
        const auto target_begin =
            std::ranges::find(plan->targets, partition.index, &SfsExtentLayoutRepairTarget::partition);
        if (target_begin == plan->targets.end())
            continue;
        const auto layout = detail::sfs_allocation_bitmap_layout(
            partition.start_sector, partition.cluster_count, partition.sectors_per_cluster, partition.bitmap_cluster);
        if (!layout)
            return std::unexpected{layout.error()};
        if (layout->rounded_bytes > std::numeric_limits<std::size_t>::max())
            return std::unexpected{repair_error("extent-layout repair bitmap exceeds the supported size")};
        std::vector<std::byte> bitmap(static_cast<std::size_t>(layout->rounded_bytes));
        if (auto read = (*source)->read_exact_at(layout->fixed_location_offset, bitmap); !read)
            return std::unexpected{read.error()};

        for (const auto &target : plan->targets) {
            if (target.partition != partition.index)
                continue;
            if (const auto checked = cancellation.check(); !checked)
                return std::unexpected{checked.error()};
            const auto record = std::ranges::find(partition.records, target.record, &IndexRecord::sfs_id);
            if (record == partition.records.end())
                return std::unexpected{repair_error("repair target record disappeared")};
            auto index = normalized_index(**source, *record, target);
            if (!index)
                return std::unexpected{index.error()};
            if (auto written = publication->write_at(record->record_offset.value, *index); !written)
                return std::unexpected{written.error()};
            if (auto written = write_continuation_lists(*publication, partition, target); !written)
                return std::unexpected{written.error()};
            if (auto released = release_trailing_allocations(bitmap, target); !released)
                return std::unexpected{released.error()};
        }
        if (auto written = publication->write_at(layout->fixed_location_offset, bitmap); !written)
            return std::unexpected{written.error()};
        if (auto written = publication->write_at(layout->header_addressed_offset, bitmap); !written)
            return std::unexpected{written.error()};
    }
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};

    // Close the validation reader before Windows reopens the staging file with delete access for publication.
    {
        auto repaired = open_image(publication->path(), options);
        if (!repaired)
            return std::unexpected{repaired.error()};
        if (!std::ranges::all_of(repaired->partitions(), [](const Partition &partition) {
                return allocation_is_safe_for_mutation(partition.allocation);
            }))
            return std::unexpected{repair_error("repaired image did not pass complete SFS allocation validation")};
        for (const auto &payload : payloads) {
            auto repaired_payload =
                repaired->read_record_data(payload.partition, payload.record, payload.bytes.size(), cancellation);
            if (!repaired_payload || !std::ranges::equal(payload.bytes, *repaired_payload)) {
                return std::unexpected{repair_error("repaired record payload differs from the source logical payload")};
            }
        }
    }
    auto published = publication->publish(overwrite ? detail::PublicationMode::replace_existing
                                                    : detail::PublicationMode::create_only);
    if (!published)
        return std::unexpected{published.error()};
    return SfsExtentLayoutRepairResult{source_path, output_path, std::move(plan->targets), std::move(*published)};
}

} // namespace axk
