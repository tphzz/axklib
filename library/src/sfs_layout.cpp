#include "sfs_internal.hpp"

#include <algorithm>

#include "axklib/bytes.hpp"

namespace axk {
Result<SfsLayout> inspect_sfs_layout(const RandomAccessReader &reader, const CancellationToken &cancellation) {
    const auto invalid = [](std::string message) {
        return std::unexpected(
            make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container, std::move(message)));
    };
    auto primary = sfs_detail::read_bytes(reader, 0U, 512U, cancellation);
    if (!primary)
        return std::unexpected(primary.error());
    auto header = sfs_detail::parse_superblock(*primary);
    if (!header)
        return std::unexpected(header.error());
    if (header->sector_size_bytes != 512U)
        return invalid("Device SFS layout requires 512-byte sectors");
    const auto declared = static_cast<std::uint64_t>(header->total_sector_count) * 512U;
    if (declared < 1536U || declared > reader.size())
        return invalid("Declared SFS extent exceeds the device view");
    auto backup = sfs_detail::read_bytes(reader, 512U, 512U, cancellation);
    if (!backup)
        return std::unexpected(backup.error());
    if (*primary != *backup)
        return invalid("SFS superblock copies differ");
    SfsLayout result{512U, declared, {}};
    OpenOptions options;
    options.cancellation = cancellation;
    for (const auto &entry : header->partition_entries) {
        if (!entry.active())
            continue;
        const auto begin = static_cast<std::uint64_t>(entry.start_sector);
        const auto end = begin + entry.sector_count;
        if (begin < 3U || begin >= end || end > declared / 512U ||
            std::ranges::any_of(result.partitions, [&](const auto &partition) {
                return begin < static_cast<std::uint64_t>(partition.start_sector) + partition.sector_count &&
                       partition.start_sector < end;
            }))
            return invalid("SFS partition bounds overlap or exceed the declared filesystem extent");
        Partition partition;
        partition.index = entry.index;
        partition.start_sector = entry.start_sector;
        partition.sector_count = entry.sector_count;
        const auto geometry = sfs_detail::read_partition_geometry(reader, partition, 512U, options, true);
        if (!geometry)
            return std::unexpected(geometry.error());
        if (!partition.backup_header_matches || !partition.diagnostics.empty())
            return invalid("SFS partition header copies or metadata geometry are inconsistent");
        result.partitions.push_back(
            {partition.index, std::move(partition.name), partition.start_sector, partition.sector_count});
    }
    if (result.partitions.empty())
        return invalid("SFS has no active partitions");
    return result;
}
} // namespace axk
