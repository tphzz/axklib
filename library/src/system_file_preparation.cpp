#include "axklib/filesystem_transaction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "axklib/system_file.hpp"
#include "sfs_files_internal.hpp"

namespace axk::detail {

Result<PreparedFilesystemEdits> prepare_sfs_system_file(std::shared_ptr<const RandomAccessReader> source,
                                                        PartitionIndex index, const SystemFilePatch &patch,
                                                        ASeriesModel model, const CancellationToken &cancellation) {
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (!source)
        return std::unexpected{sfs_files::error("System File source is required")};
    if (model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{
            sfs_files::error("System File target model is unsupported", ErrorCode::unsupported_profile)};
    OpenOptions options;
    options.cancellation = cancellation;
    const auto image = open_image(source, {}, options);
    if (!image)
        return std::unexpected{image.error()};
    if (auto checked = sfs_files::validate(*image, index); !checked)
        return std::unexpected{checked.error()};
    const auto &partition = *std::ranges::find(image->partitions(), index, &Partition::index);
    const auto kind = model == ASeriesModel::a3000 ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2;
    const auto id = locate_system_file_record(partition, kind);
    if (!id)
        return std::unexpected{id.error()};
    if (!*id)
        return std::unexpected{sfs_files::error("The partition has no saved System File for the target model")};
    const auto &record = *std::ranges::find(partition.records, **id, &IndexRecord::sfs_id);
    if (record.link_count != 1U)
        return std::unexpected{sfs_files::error("System File has aliases; targeted editing requires a unique file")};
    if (record.data_size != system_file_record_size(kind))
        return std::unexpected{sfs_files::error("System File record size is invalid")};
    const auto original = image->read_record_data(index, **id, system_file_record_size(kind));
    if (!original)
        return std::unexpected{original.error()};
    const auto decoded = decode_system_file(kind, *original);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto changed = patch_system_file(*decoded, patch, model);
    if (!changed)
        return std::unexpected{changed.error()};
    const auto encoded = encode_system_file(*changed);
    if (!encoded)
        return std::unexpected{encoded.error()};
    const auto contents = std::make_shared<MemoryReader>(*encoded);
    const auto sector_bytes = image->superblock().sector_size_bytes;
    const auto cluster_bytes = static_cast<std::uint64_t>(partition.sectors_per_cluster) * sector_bytes;
    const auto partition_offset = static_cast<std::uint64_t>(partition.start_sector) * sector_bytes;
    std::vector<FilesystemWritePatch> writes;
    std::size_t logical{};
    for (const auto &extent : record.extents) {
        const auto count = std::min<std::size_t>(extent.byte_count, encoded->size() - logical);
        const auto physical = partition_offset + static_cast<std::uint64_t>(extent.cluster_offset) * cluster_bytes;
        for (std::size_t within = 0; within < count;) {
            if ((*original)[logical + within] == (*encoded)[logical + within]) {
                ++within;
                continue;
            }
            const auto start = within++;
            while (within < count && (*original)[logical + within] != (*encoded)[logical + within])
                ++within;
            writes.push_back({physical + start, contents, logical + start, within - start});
        }
        logical += count;
    }
    if (logical != encoded->size())
        return std::unexpected{sfs_files::error("System File extents do not cover the complete payload")};
    auto normalized =
        normalize_filesystem_patches(*source, writes, partition_offset,
                                     static_cast<std::uint64_t>(partition.sector_count) * sector_bytes, cancellation);
    if (!normalized)
        return std::unexpected{normalized.error()};
    auto preview = filesystem_preview(source, *normalized);
    const auto reopened = open_image(preview, {}, options);
    if (!reopened)
        return std::unexpected{reopened.error()};
    if (auto checked = sfs_files::validate(*reopened, index); !checked)
        return std::unexpected{checked.error()};
    const auto verified = reopened->read_record_data(index, **id, encoded->size());
    if (!verified)
        return std::unexpected{verified.error()};
    if (*verified != *encoded)
        return std::unexpected{sfs_files::error("System File preview differs from the validated patch")};
    return PreparedFilesystemEdits{source->size(), std::move(*normalized), std::move(preview)};
}

} // namespace axk::detail
