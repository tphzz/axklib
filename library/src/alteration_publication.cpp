#include "alteration_internal.hpp"

#include "axklib/bytes.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sfs_allocation.hpp"
#include "axklib/writer_internal.hpp"
#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
namespace axk::alteration_internal {
Result<void> write_bytes(detail::TemporaryPublication &publication, std::uint64_t offset,
                         std::span<const std::byte> data) {
    return publication.write_at(offset, data);
}
Result<std::vector<std::byte>> continuation_list_bytes(const MutablePartition::InsertedRecord &record,
                                                       std::size_t list_index) {
    constexpr std::size_t extents_per_cluster = (1024U - 12U) / 12U;
    const auto extent_begin = list_index * extents_per_cluster;
    if (extent_begin >= record.extents.size())
        return std::unexpected{transaction_error("continuation list has no extents")};
    const auto extent_count = std::min(extents_per_cluster, record.extents.size() - extent_begin);
    std::vector<std::byte> block(1024U);
    ByteWriter writer{block};
    if (auto written = writer.write_be32(0U, static_cast<std::uint32_t>(extent_count)); !written)
        return std::unexpected{written.error()};
    const auto next =
        list_index + 1U < record.continuation_clusters.size() ? record.continuation_clusters[list_index + 1U] : 0U;
    if (auto written = writer.write_be32(8U, next); !written)
        return std::unexpected{written.error()};
    for (std::size_t index = 0; index < extent_count; ++index) {
        const auto &extent = record.extents[extent_begin + index];
        const auto offset = 12U + index * 12U;
        if (auto written = writer.write_be32(offset, extent.cluster_offset); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(offset + 4U, extent.cluster_count); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(offset + 8U, extent.byte_count); !written)
            return std::unexpected{written.error()};
    }
    return block;
}

Result<void> append_patch(std::vector<detail::AlterationPatch> &patches, const TransactionState &state,
                          std::uint64_t offset, std::span<const std::byte> replacement) {
    if (offset > state.source->size() || replacement.size() > state.source->size() - offset)
        return std::unexpected{transaction_error("alteration patch exceeds the source image")};
    std::vector<std::byte> original(replacement.size());
    if (auto read = state.source->read_exact_at(offset, original); !read)
        return std::unexpected{read.error()};
    if (std::ranges::equal(original, replacement))
        return {};
    patches.push_back(detail::AlterationPatch{offset, std::move(original), {replacement.begin(), replacement.end()}});
    return {};
}

Result<std::vector<detail::AlterationPatch>> collect_patches(const TransactionState &state,
                                                             const CancellationToken &cancellation) {
    std::vector<detail::AlterationPatch> patches;
    for (const auto &[index, item] : state.partitions) {
        static_cast<void>(index);
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto &partition = *item.source;
        if (item.renamed_name) {
            std::array<std::byte, 16> encoded_name{};
            std::ranges::fill(encoded_name, std::byte{' '});
            for (std::size_t name_index = 0; name_index < item.renamed_name->size(); ++name_index)
                encoded_name[name_index] = static_cast<std::byte>((*item.renamed_name)[name_index]);
            const auto header_offset = static_cast<std::uint64_t>(partition.start_sector) * 512U + 0x40U;
            if (auto appended = append_patch(patches, state, header_offset, encoded_name); !appended)
                return std::unexpected{appended.error()};
            if (auto appended = append_patch(patches, state, header_offset + 1024U, encoded_name); !appended)
                return std::unexpected{appended.error()};
        }
        for (const auto id : item.deleted) {
            const auto *source_record = record(partition, id);
            if (source_record == nullptr)
                continue;
            const std::array<std::byte, 72> zero{};
            if (auto appended = append_patch(patches, state, source_record->record_offset.value, zero); !appended)
                return std::unexpected{appended.error()};
        }
        if (item.root_index && item.root_payload) {
            const auto *root = record(partition, SfsId{1});
            if (root == nullptr)
                return std::unexpected{transaction_error("partition root record is missing")};
            if (auto appended = append_patch(patches, state, root->record_offset.value, *item.root_index); !appended)
                return std::unexpected{appended.error()};
            const auto payload_offset =
                (static_cast<std::uint64_t>(partition.start_sector) +
                 static_cast<std::uint64_t>(root->extents[0].cluster_offset) * partition.sectors_per_cluster) *
                512U;
            if (auto appended = append_patch(patches, state, payload_offset, *item.root_payload); !appended)
                return std::unexpected{appended.error()};
        }
        const auto index_base =
            (static_cast<std::uint64_t>(partition.start_sector) +
             static_cast<std::uint64_t>(partition.directory_index_cluster) * partition.sectors_per_cluster) *
            512U;
        const auto append_record = [&](const MutablePartition::InsertedRecord &changed,
                                       std::uint64_t index_offset) -> Result<void> {
            auto extents = changed.extents;
            auto raw_index = changed.raw_index;
            if (changed.capacity_expanded) {
                if (auto normalized = normalize_extent_byte_counts(extents, changed.payload.size()); !normalized)
                    return std::unexpected{normalized.error()};
                const ByteReader current_index{raw_index};
                const auto tail = current_index.be16(0x46U);
                if (!tail)
                    return std::unexpected{tail.error()};
                detail::PreparedRecord prepared;
                prepared.kind = changed.payload_kind == PayloadKind::directory ? detail::RecordKind::directory
                                                                               : detail::RecordKind::object;
                prepared.tail = *tail;
                auto encoded = detail::encode_sfs_index_record(prepared, extents,
                                                               static_cast<std::uint32_t>(changed.payload.size()),
                                                               changed.continuation_clusters);
                if (!encoded)
                    return std::unexpected{encoded.error()};
                raw_index = std::move(*encoded);
            }
            if (auto appended = append_patch(patches, state, index_offset, raw_index); !appended)
                return appended;
            std::size_t payload_offset{};
            for (const auto &extent : extents) {
                const auto capacity = static_cast<std::size_t>(extent.cluster_count) * 1024U;
                const auto count = static_cast<std::size_t>(extent.byte_count);
                if (count == 0U || count > capacity || payload_offset > changed.payload.size() ||
                    count > changed.payload.size() - payload_offset) {
                    return std::unexpected{transaction_error("record extent byte total differs from its payload")};
                }
                const auto absolute =
                    (static_cast<std::uint64_t>(partition.start_sector) +
                     static_cast<std::uint64_t>(extent.cluster_offset) * partition.sectors_per_cluster) *
                    512U;
                if (auto appended = append_patch(patches, state, absolute,
                                                 std::span{changed.payload}.subspan(payload_offset, count));
                    !appended) {
                    return appended;
                }
                payload_offset += count;
            }
            if (payload_offset != changed.payload.size())
                return std::unexpected{transaction_error("record extent byte total differs from its payload")};
            const MutablePartition::InsertedRecord materialized{
                changed.id, {}, {}, extents, changed.continuation_clusters, changed.payload_kind, false};
            for (std::size_t list_index = 0; list_index < changed.continuation_clusters.size(); ++list_index) {
                auto bytes = continuation_list_bytes(materialized, list_index);
                if (!bytes)
                    return std::unexpected{bytes.error()};
                const auto absolute = (static_cast<std::uint64_t>(partition.start_sector) +
                                       static_cast<std::uint64_t>(changed.continuation_clusters[list_index]) *
                                           partition.sectors_per_cluster) *
                                      512U;
                if (auto appended = append_patch(patches, state, absolute, *bytes); !appended)
                    return appended;
            }
            return {};
        };
        for (const auto &[id, changed] : item.changed) {
            const auto *source_record = record(partition, id);
            if (source_record == nullptr)
                return std::unexpected{transaction_error("changed record has no source index location")};
            if (auto appended = append_record(changed, source_record->record_offset.value); !appended)
                return std::unexpected{appended.error()};
        }
        for (const auto &[id, inserted] : item.inserted) {
            const auto index_offset = index_base + (id.value / 14U) * 1024U + (id.value % 14U) * 72U;
            if (auto appended = append_record(inserted, index_offset); !appended)
                return std::unexpected{appended.error()};
        }
        const auto bitmap_layout = detail::sfs_allocation_bitmap_layout(
            partition.start_sector, partition.cluster_count, partition.sectors_per_cluster, partition.bitmap_cluster);
        if (!bitmap_layout || bitmap_layout->rounded_bytes != item.bitmap.size())
            return std::unexpected{transaction_error("alteration allocation bitmap geometry is inconsistent")};
        if (auto appended = append_patch(patches, state, bitmap_layout->header_addressed_offset, item.bitmap);
            !appended)
            return std::unexpected{appended.error()};
        if (auto appended = append_patch(patches, state, bitmap_layout->fixed_location_offset, item.bitmap);
            !appended) {
            return std::unexpected{appended.error()};
        }
    }
    std::ranges::sort(patches, {}, &detail::AlterationPatch::offset);
    for (std::size_t index = 1U; index < patches.size(); ++index) {
        const auto &previous = patches[index - 1U];
        if (previous.offset + previous.replacement.size() > patches[index].offset)
            return std::unexpected{transaction_error("alteration patch ranges overlap")};
    }
    return patches;
}

Result<detail::TemporaryPublication> copy_to_unique_temporary(const RandomAccessReader &source,
                                                              const std::filesystem::path &output) {
    auto publication = detail::TemporaryPublication::create(output);
    if (!publication)
        return std::unexpected{publication.error()};
    if (auto resized = publication->resize(source.size()); !resized) {
        return std::unexpected{resized.error()};
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), 1024U * 1024U)));
    for (std::uint64_t offset = 0U; offset < source.size(); offset += buffer.size()) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - offset));
        auto bytes = std::span{buffer}.first(count);
        if (auto read = source.read_exact_at(offset, bytes); !read) {
            return std::unexpected{read.error()};
        }
        if (auto written = publication->write_at(offset, bytes); !written) {
            return std::unexpected{written.error()};
        }
    }
    return std::move(*publication);
}

Result<void> validate_temporary(const std::filesystem::path &temporary, const TransactionState &state,
                                const CancellationToken &cancellation) {
    OpenOptions options;
    options.cancellation = cancellation;
    auto actual = open_image(temporary, options);
    if (!actual)
        return std::unexpected{actual.error()};
    if (auto placements = validate_post_write_placements(state, *actual, cancellation); !placements)
        return placements;
    auto expected_patches = collect_patches(state, cancellation);
    if (!expected_patches)
        return std::unexpected{expected_patches.error()};
    auto written_image = FileReader::open(temporary);
    if (!written_image)
        return std::unexpected{written_image.error()};
    if ((*written_image)->size() != state.source->size())
        return std::unexpected{transaction_error("post-write image size differs from the planned snapshot")};
    constexpr std::size_t comparison_chunk_size = 1024U * 1024U;
    std::vector<std::byte> source_bytes(comparison_chunk_size);
    std::vector<std::byte> written_bytes(comparison_chunk_size);
    std::size_t patch_index{};
    for (std::uint64_t offset = 0U; offset < state.source->size();) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(comparison_chunk_size, state.source->size() - offset));
        auto expected = std::span{source_bytes}.first(count);
        auto observed = std::span{written_bytes}.first(count);
        if (auto read = state.source->read_exact_at(offset, expected); !read)
            return std::unexpected{read.error()};
        if (auto read = (*written_image)->read_exact_at(offset, observed); !read)
            return std::unexpected{read.error()};
        while (patch_index < expected_patches->size() &&
               (*expected_patches)[patch_index].offset + (*expected_patches)[patch_index].replacement.size() <=
                   offset) {
            ++patch_index;
        }
        for (auto index = patch_index; index < expected_patches->size(); ++index) {
            const auto &patch = (*expected_patches)[index];
            if (patch.offset >= offset + count)
                break;
            const auto overlap_begin = std::max(patch.offset, offset);
            const auto overlap_end = std::min<std::uint64_t>(patch.offset + patch.replacement.size(), offset + count);
            const auto replacement_offset = static_cast<std::size_t>(overlap_begin - patch.offset);
            const auto destination_offset = static_cast<std::size_t>(overlap_begin - offset);
            const auto overlap_size = static_cast<std::size_t>(overlap_end - overlap_begin);
            std::ranges::copy(std::span{patch.replacement}.subspan(replacement_offset, overlap_size),
                              expected.begin() + static_cast<std::ptrdiff_t>(destination_offset));
        }
        if (!std::ranges::equal(expected, observed))
            return std::unexpected{transaction_error("post-write image differs from the complete planned snapshot")};
        offset += count;
    }
    if (actual->partitions().size() != state.container.partitions().size())
        return std::unexpected{transaction_error("post-write validation changed the partition set")};
    for (const auto &[index, expected] : state.partitions) {
        const auto partition = std::ranges::find(actual->partitions(), PartitionIndex{index}, &Partition::index);
        if (partition == actual->partitions().end())
            return std::unexpected{transaction_error("post-write validation lost a planned partition")};
        if (!allocation_is_safe_for_mutation(partition->allocation)) {
            return std::unexpected{transaction_error("post-write allocation validation is not clean")};
        }
        if (expected.renamed_name && (partition->name != *expected.renamed_name || !partition->backup_header_matches)) {
            return std::unexpected{transaction_error("post-write partition name differs from the transaction plan")};
        }
        std::set<SfsId> expected_ids;
        for (const auto &source_record : expected.source->records) {
            if (!expected.deleted.contains(source_record.sfs_id))
                expected_ids.insert(source_record.sfs_id);
        }
        for (const auto &[id, inserted] : expected.inserted) {
            static_cast<void>(inserted);
            expected_ids.insert(id);
        }
        std::set<SfsId> actual_ids;
        for (const auto &actual_record : partition->records)
            actual_ids.insert(actual_record.sfs_id);
        if (actual_ids != expected_ids)
            return std::unexpected{transaction_error("post-write SFS record set differs from the transaction plan")};

        const auto compare_payload = [&](SfsId id, std::span<const std::byte> payload) -> Result<void> {
            auto written = actual->read_record_data(partition->index, id, 64U * 1024U * 1024U, cancellation);
            if (!written)
                return std::unexpected{written.error()};
            if (!std::ranges::equal(*written, payload))
                return std::unexpected{transaction_error("post-write object payload differs from "
                                                         "the transaction plan")};
            return {};
        };
        if (expected.root_payload) {
            if (auto compared = compare_payload(SfsId{1}, *expected.root_payload); !compared)
                return compared;
        }
        for (const auto &[id, changed] : expected.changed) {
            if (auto compared = compare_payload(id, changed.payload); !compared)
                return compared;
        }
        for (const auto &[id, inserted] : expected.inserted) {
            if (auto compared = compare_payload(id, inserted.payload); !compared)
                return compared;
        }
        for (const auto &source_record : expected.source->records) {
            if (expected.deleted.contains(source_record.sfs_id) || expected.changed.contains(source_record.sfs_id) ||
                expected.inserted.contains(source_record.sfs_id) ||
                (source_record.sfs_id.value == 1U && expected.root_payload)) {
                continue;
            }
            const auto written_record =
                std::ranges::find(partition->records, source_record.sfs_id, &IndexRecord::sfs_id);
            if (written_record == partition->records.end())
                return std::unexpected{transaction_error("post-write validation lost an unchanged SFS record")};
            std::array<std::byte, 72> source_index{};
            if (auto read = state.source->read_exact_at(source_record.record_offset.value, source_index); !read)
                return std::unexpected{read.error()};
            auto written_index = read_raw(temporary, written_record->record_offset.value, 72U);
            if (!written_index)
                return std::unexpected{written_index.error()};
            if (!std::ranges::equal(source_index, *written_index))
                return std::unexpected{transaction_error("post-write validation changed untouched SFS ID " +
                                                         std::to_string(source_record.sfs_id.value) + " index record")};
            for (std::uint64_t offset = 0U; offset < source_record.data_size;) {
                const auto count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(comparison_chunk_size, source_record.data_size - offset));
                auto source_data = state.container.read_record_range(expected.source->index, source_record.sfs_id,
                                                                     offset, count, cancellation);
                if (!source_data)
                    return std::unexpected{source_data.error()};
                auto written_data =
                    actual->read_record_range(partition->index, source_record.sfs_id, offset, count, cancellation);
                if (!written_data)
                    return std::unexpected{written_data.error()};
                if (*source_data != *written_data)
                    return std::unexpected{transaction_error("post-write validation changed untouched SFS ID " +
                                                             std::to_string(source_record.sfs_id.value) + " payload")};
                offset += count;
            }
        }
        const auto bitmap_layout =
            detail::sfs_allocation_bitmap_layout(partition->start_sector, partition->cluster_count,
                                                 partition->sectors_per_cluster, partition->bitmap_cluster);
        if (!bitmap_layout || bitmap_layout->rounded_bytes > std::numeric_limits<std::size_t>::max())
            return std::unexpected{transaction_error("post-write allocation bitmap exceeds platform limits")};
        auto bitmap = read_raw(temporary, bitmap_layout->header_addressed_offset,
                               static_cast<std::size_t>(bitmap_layout->rounded_bytes));
        if (!bitmap)
            return std::unexpected{bitmap.error()};
        if (*bitmap != expected.bitmap)
            return std::unexpected{transaction_error("post-write allocation bitmap differs from "
                                                     "the transaction plan")};
    }
    return {};
}

Result<void> validate_mutable_partition_geometry(const Partition &partition, std::uint64_t image_size_bytes) {
    if (partition.cluster_count == 0U || partition.sectors_per_cluster == 0U ||
        partition.directory_index_span_clusters == 0U) {
        return std::unexpected{transaction_error("source partition has incomplete allocation geometry")};
    }
    if (partition.sectors_per_cluster != 2U) {
        return std::unexpected{
            transaction_error("source partition geometry is outside the supported alteration profile")};
    }
    const auto bitmap_bytes = (static_cast<std::uint64_t>(partition.cluster_count) + 7U) / 8U;
    const auto cluster_bytes = static_cast<std::uint64_t>(partition.sectors_per_cluster) * 512U;
    const auto bitmap_span = (bitmap_bytes + cluster_bytes - 1U) / cluster_bytes;
    const auto bitmap_end = static_cast<std::uint64_t>(partition.bitmap_cluster) + bitmap_span;
    const auto index_end =
        static_cast<std::uint64_t>(partition.directory_index_cluster) + partition.directory_index_span_clusters;
    const auto physical_cluster_capacity =
        static_cast<std::uint64_t>(partition.sector_count) / partition.sectors_per_cluster;
    const auto partition_end_sector = static_cast<std::uint64_t>(partition.start_sector) + partition.sector_count;
    if (bitmap_span == 0U || partition.cluster_count > physical_cluster_capacity ||
        bitmap_end > partition.cluster_count || index_end > partition.cluster_count ||
        !(bitmap_end <= partition.directory_index_cluster || index_end <= partition.bitmap_cluster) ||
        partition_end_sector > image_size_bytes / 512U) {
        return std::unexpected{transaction_error("source allocation geometry cannot safely support alteration")};
    }
    return {};
}

Result<TransactionState> open_transaction_state(std::shared_ptr<const RandomAccessReader> source,
                                                const std::filesystem::path &source_path,
                                                const CancellationToken &cancellation, ProgressSink *progress,
                                                bool include_object_graph,
                                                std::optional<PartitionIndex> extent_repair_partition) {
    if (!source)
        return std::unexpected{transaction_error("alteration source reader is required")};
    OpenOptions options;
    options.cancellation = cancellation;
    options.progress = progress;
    auto container = open_image(source, source_path, options);
    if (!container)
        return std::unexpected{container.error()};
    TransactionState state{std::move(source), std::move(*container), {}, {}, {}, {}, {}};
    if (state.container.superblock().sector_size_bytes != 512U) {
        return std::unexpected{
            transaction_error("source sector size is outside the supported 512-byte alteration profile")};
    }
    if (std::ranges::any_of(state.container.diagnostics(), [](const Error &error) {
            return error.code == ErrorCode::container_invalid_geometry ||
                   error.code == ErrorCode::container_partition_out_of_range;
        })) {
        return std::unexpected{transaction_error("source allocation geometry cannot safely support alteration")};
    }
    if (include_object_graph) {
        auto catalog = build_object_catalog(state.container, 64U * 1024U * 1024U, cancellation);
        if (!catalog)
            return std::unexpected{catalog.error()};
        state.catalog = std::move(*catalog);
        state.graph = build_relationship_graph(state.catalog);
    }
    std::map<std::string, std::pair<PartitionIndex, SfsId>> object_ids;
    for (const auto &object : state.catalog.objects)
        object_ids.emplace(object.key, std::pair{object.partition, object.sfs_id});
    for (const auto &relationship : state.graph.relationships) {
        if (relationship.quality != RelationshipQuality::known || !relationship.target_key)
            continue;
        const auto source_object = object_ids.find(relationship.source_key);
        const auto target = object_ids.find(*relationship.target_key);
        if (source_object != object_ids.end() && target != object_ids.end() &&
            source_object->second.first == target->second.first) {
            state.known_edges.emplace_back(source_object->second.first, source_object->second.second,
                                           target->second.second);
        }
    }
    for (const auto &partition : state.container.partitions()) {
        if (auto geometry = validate_mutable_partition_geometry(partition, state.container.image_size_bytes());
            !geometry) {
            return std::unexpected{geometry.error()};
        }
        const auto explicitly_repairable =
            extent_repair_partition == partition.index && placement_repair_can_normalize_directory_extents(partition);
        if (!allocation_is_safe_for_mutation(partition.allocation) && !explicitly_repairable) {
            return std::unexpected{transaction_error("source allocation cannot safely support alteration")};
        }
        const auto bitmap_layout = detail::sfs_allocation_bitmap_layout(
            partition.start_sector, partition.cluster_count, partition.sectors_per_cluster, partition.bitmap_cluster);
        if (!bitmap_layout || bitmap_layout->rounded_bytes > std::numeric_limits<std::size_t>::max())
            return std::unexpected{transaction_error("source allocation bitmap exceeds platform limits")};
        std::vector<std::byte> bitmap(static_cast<std::size_t>(bitmap_layout->rounded_bytes));
        if (auto read = state.source->read_exact_at(bitmap_layout->header_addressed_offset, bitmap); !read)
            return std::unexpected{read.error()};
        MutablePartition mutable_partition;
        mutable_partition.source = &partition;
        mutable_partition.bitmap = std::move(bitmap);
        if (explicitly_repairable) {
            if (auto staged = stage_recoverable_directory_extent_repairs(*state.source, partition, mutable_partition,
                                                                         cancellation);
                !staged) {
                return std::unexpected{staged.error()};
            }
        }
        state.partitions.emplace(partition.index.value, std::move(mutable_partition));
    }
    return state;
}

Result<TransactionState> open_transaction_state(const std::filesystem::path &source_path,
                                                const CancellationToken &cancellation, ProgressSink *progress,
                                                bool include_object_graph,
                                                std::optional<PartitionIndex> extent_repair_partition) {
    auto source = FileReader::open(source_path);
    if (!source)
        return std::unexpected{source.error()};
    return open_transaction_state(*source, source_path, cancellation, progress, include_object_graph,
                                  extent_repair_partition);
}

Result<PublicationOutcome>
publish(const TransactionState &state, const std::filesystem::path &output_path, const CancellationToken &cancellation,
        bool overwrite, const std::function<Result<void>(const std::filesystem::path &)> &temporary_validator,
        ProgressSink *progress) {
    if (!overwrite && std::filesystem::exists(output_path))
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "alteration output already exists")};
    std::error_code error;
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), error);
    if (error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create alteration output directory")};
    if (progress) {
        progress->report(
            {ProgressPhase::writing, 0U, state.partitions.size(), "writing alteration image", output_path.string()});
    }
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    if (auto distinct = require_distinct_source_and_output(state.container.source_path(), output_path, "alteration");
        !distinct) {
        return std::unexpected{distinct.error()};
    }
    auto temporary_result = copy_to_unique_temporary(*state.source, output_path);
    if (!temporary_result)
        return std::unexpected{temporary_result.error()};
    auto publication = std::move(*temporary_result);
    auto patches = collect_patches(state, cancellation);
    if (!patches)
        return std::unexpected{patches.error()};
    std::uint64_t completed_patches{};
    for (const auto &patch : *patches) {
        if (const auto check = cancellation.check(); !check) {
            return std::unexpected{check.error()};
        }
        if (auto written = write_bytes(publication, patch.offset, patch.replacement); !written) {
            return std::unexpected{written.error()};
        }
        ++completed_patches;
        if (progress) {
            progress->report({ProgressPhase::writing, completed_patches, patches->size(), "writing alteration image",
                              output_path.string()});
        }
    }
    if (auto flushed = publication.flush(); !flushed) {
        return std::unexpected{flushed.error()};
    }
    if (const auto check = cancellation.check(); !check) {
        return std::unexpected{check.error()};
    }
    if (auto distinct = require_distinct_source_and_output(state.container.source_path(), output_path, "alteration");
        !distinct) {
        return std::unexpected{distinct.error()};
    }
    auto validated = validate_temporary(publication.path(), state, cancellation);
    if (!validated) {
        return std::unexpected{validated.error()};
    }
    if (temporary_validator) {
        auto package_validated = temporary_validator(publication.path());
        if (!package_validated) {
            return std::unexpected{package_validated.error()};
        }
    }
    if (const auto check = cancellation.check(); !check) {
        return std::unexpected{check.error()};
    }
    const auto mode = overwrite ? detail::PublicationMode::replace_existing : detail::PublicationMode::create_only;
    auto published = publication.publish(mode);
    if (!published) {
        return std::unexpected{published.error()};
    }
    return std::move(*published);
}

} // namespace axk::alteration_internal
