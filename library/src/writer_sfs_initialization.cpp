#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>

#include "axklib/bytes.hpp"
#include "axklib/sfs_allocation.hpp"

namespace axk::detail {
namespace {
constexpr std::uint64_t cluster_size = 1024U;
}

Result<std::vector<std::byte>> sfs_ascii(std::string_view value, std::size_t size, std::byte pad) {
    if (value.size() > size || !std::ranges::all_of(value, [](unsigned char character) { return character < 0x80U; })) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "writer name does not fit its ASCII field")};
    }
    std::vector<std::byte> result(size, pad);
    std::ranges::transform(value, result.begin(), [](char character) { return static_cast<std::byte>(character); });
    return result;
}

Result<WrittenImageLayout>
emit_hds_records(const HdsBuildManifest &manifest, const std::vector<PartitionGeometry> &partition_geometry,
                 const std::vector<std::vector<PreparedRecord>> &all_records, const std::filesystem::path &output_path,
                 const std::function<Result<void>(std::uint64_t, std::span<const std::byte>)> &write_at,
                 const CancellationToken &cancellation) {
    const auto *geometries = &partition_geometry;
    std::vector<std::byte> superblock(512);
    const std::string_view magic{"YAMAHA_dev3"};
    std::ranges::transform(magic, superblock.begin(), [](char value) { return static_cast<std::byte>(value); });
    constexpr std::array<std::uint32_t, 7> residue{0xa1e00152, 0xa22c0000, 0x17,      0x09100000,
                                                   0x17,       0x09100000, 0x01000152};
    ByteWriter superblock_writer{superblock};
    for (std::size_t index = 0; index < residue.size(); ++index) {
        if (auto written = superblock_writer.write_be32(0x80 + index * 4U, residue[index]); !written)
            return std::unexpected{written.error()};
    }
    if (auto written = superblock_writer.write_be32(0x9c, 512); !written)
        return std::unexpected{written.error()};
    if (auto written = superblock_writer.write_be32(0xa0, static_cast<std::uint32_t>(manifest.size_bytes / 512U));
        !written)
        return std::unexpected{written.error()};
    for (const auto &geometry : *geometries) {
        if (auto written = superblock_writer.write_be32(0xa8 + geometry.index * 8U,
                                                        static_cast<std::uint32_t>(geometry.start_sector));
            !written)
            return std::unexpected{written.error()};
        if (auto written = superblock_writer.write_be32(0xac + geometry.index * 8U,
                                                        static_cast<std::uint32_t>(geometry.filesystem_sector_count));
            !written)
            return std::unexpected{written.error()};
    }
    if (!write_at(0, superblock) || !write_at(512, superblock)) {
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write HDS superblocks")};
    }
    WrittenImageLayout layout{output_path, manifest.size_bytes, {}, 0, {}};
    for (std::size_t partition_index = 0; partition_index < geometries->size(); ++partition_index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto &geometry = (*geometries)[partition_index];
        const auto &records = all_records[partition_index];
        const std::vector<std::byte> reserved(static_cast<std::size_t>(geometry.first_payload_cluster * cluster_size));
        if (auto cleared = write_at(geometry.start_sector * 512U, reserved); !cleared)
            return std::unexpected{cleared.error()};
        std::vector<std::byte> token(512);
        const auto token_text = std::format("{:08x}", 0xab432100U | geometry.index);
        std::ranges::transform(token_text, token.begin(), [](char value) { return static_cast<std::byte>(value); });
        const auto token_offset = geometry.index == 0 ? 1024U : (geometry.start_sector - 1U) * 512U;
        if (!write_at(token_offset, token))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write HDS transfer token")};
        std::vector<std::byte> header(1024);
        std::ranges::transform(magic, header.begin(), [](char value) { return static_cast<std::byte>(value); });
        auto name = sfs_ascii(manifest.partitions[partition_index].name, 16U);
        if (!name)
            return std::unexpected{name.error()};
        std::ranges::copy(*name, header.begin() + 0x40);
        ByteWriter header_writer{header};
        if (auto written = header_writer.write_be32(0x80, 2); !written)
            return std::unexpected{written.error()};
        if (auto written = header_writer.write_be32(0x84, 200); !written)
            return std::unexpected{written.error()};
        std::fill(header.begin() + 0x88, header.begin() + 0x90, std::byte{0xff});
        for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 7>{
                 {{0x90, static_cast<std::uint32_t>(geometry.cluster_count)},
                  {0x94, detail::sfs_initial_bitmap_cluster},
                  {0x98, detail::sfs_initial_bitmap_cluster},
                  {0x9c, static_cast<std::uint32_t>(geometry.bitmap_cluster)},
                  {0xa0, detail::sfs_directory_index_capacity},
                  {0xa4, static_cast<std::uint32_t>(geometry.directory_index_cluster)},
                  {0xa8, detail::sfs_directory_index_page_capacity}}}) {
            if (auto written = header_writer.write_be32(offset, value); !written)
                return std::unexpected{written.error()};
        }
        const auto dynamic = 0x0152a3fcU + geometry.index * 0x11U;
        const auto total_sectors = manifest.size_bytes / 512U;
        if (geometries->size() == 2U)
            header[0xaf] = static_cast<std::byte>(geometry.index);
        for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 17>{
                 {{0x104, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x114, geometry.index * 0x20U},
                  {0x118, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x11c, static_cast<std::uint32_t>(geometry.filesystem_sector_count)},
                  {0x134, dynamic},
                  {0x144, geometry.index == 0 ? 0x016f5bf0U : 0x015d6cc0U},
                  {0x14c, static_cast<std::uint32_t>(
                              geometries->size() >= 3U ? total_sectors - (geometries->size() - 2U) : total_sectors)},
                  {0x154, geometry.index},
                  {0x158, dynamic},
                  {0x15c, geometry.index * 0x20U},
                  {0x160, static_cast<std::uint32_t>(geometry.index * geometry.filesystem_sector_count)},
                  {0x164, dynamic},
                  {0x178, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x17c, static_cast<std::uint32_t>(geometry.filesystem_sector_count)},
                  {0x184, dynamic},
                  {0x194, geometries->size() >= 3U ? 0U : static_cast<std::uint32_t>(geometries->size())},
                  {0x1a8, geometry.index}}}) {
            if (auto written = header_writer.write_be32(offset, value); !written)
                return std::unexpected{written.error()};
        }
        const auto start = geometry.start_sector * 512U;
        if (!write_at(start, header) || !write_at(start + 1024U, header))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition headers")};
        std::vector<std::byte> bitmap(geometry.bitmap_cluster_count * cluster_size);
        auto index_size = detail::checked_directory_index_size(records);
        if (!index_size)
            return std::unexpected{index_size.error()};
        std::vector<std::byte> index(*index_size);
        std::uint64_t allocated{};
        for (const auto &record : records) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            allocated += record.clusters;
            const auto record_end = static_cast<std::uint64_t>(record.cluster) + record.clusters;
            if (record_end > geometry.cluster_count) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "prepared SFS record exceeds the partition cluster range")};
            }
            for (std::uint32_t cluster = record.cluster; cluster < record_end; ++cluster) {
                const auto byte = static_cast<std::size_t>(cluster / 8U);
                if (byte >= bitmap.size()) {
                    return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                      "prepared SFS allocation exceeds the bitmap")};
                }
                bitmap[byte] |= static_cast<std::byte>(0x80U >> (cluster % 8U));
            }
            const auto offset =
                (record.id / detail::sfs_directory_index_records_per_page) * detail::sfs_directory_index_page_bytes +
                (record.id % detail::sfs_directory_index_records_per_page) * detail::sfs_directory_index_record_bytes;
            auto bytes = detail::encode_sfs_index_record(record);
            if (!bytes)
                return std::unexpected{bytes.error()};
            if (offset > index.size() || bytes->size() > index.size() - offset) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "prepared SFS directory record exceeds the index")};
            }
            std::ranges::copy(*bytes, std::span{index}.subspan(offset, bytes->size()).begin());
            if (record.kind != RecordKind::object && record.clusters != 0U) {
                const std::vector<std::byte> padding(static_cast<std::size_t>(record.clusters) * cluster_size);
                if (auto cleared = write_at((geometry.start_sector + record.cluster * 2U) * 512U, padding); !cleared)
                    return std::unexpected{cleared.error()};
            }
            if (!record.payload.empty() &&
                !write_at((geometry.start_sector + record.cluster * 2U) * 512U, record.payload))
                return std::unexpected{
                    make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition payload")};
        }
        const auto bitmap_layout = detail::sfs_allocation_bitmap_layout(
            geometry.start_sector, static_cast<std::uint32_t>(geometry.cluster_count), 2U,
            detail::sfs_initial_bitmap_cluster, static_cast<std::uint32_t>(geometry.bitmap_cluster));
        if (!bitmap_layout || bitmap_layout->rounded_bytes != bitmap.size()) {
            return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                              "prepared SFS allocation bitmap geometry is inconsistent")};
        }
        if (!write_at(bitmap_layout->bitmap_copy1_offset, bitmap) ||
            !write_at(bitmap_layout->bitmap_copy2_offset, bitmap) ||
            !write_at((geometry.start_sector + geometry.directory_index_cluster * 2U) * 512U, index))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition allocation data")};
        const auto free_clusters = geometry.cluster_count - geometry.first_payload_cluster - allocated;
        layout.partitions.push_back({geometry, manifest.partitions[partition_index].name, allocated, free_clusters,
                                     free_clusters * 1024U, (free_clusters * 1024U) / 1024U});
    }
    const auto &last = partition_geometry.back();
    layout.unused_tail_sectors = manifest.size_bytes / 512U - (last.start_sector + last.filesystem_sector_count);
    return layout;
}
} // namespace axk::detail

namespace axk {
Result<HdsInitialization> plan_hds_initialization(std::uint64_t size_bytes, std::uint8_t count,
                                                  const CancellationToken &cancellation) {
    auto options = hds_creation_partition_options(size_bytes);
    if (!options)
        return std::unexpected{options.error()};
    const auto selected = std::ranges::find(*options, count, &HdsCreationPartitionOption::partition_count);
    if (selected == options->end())
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "partition count is unavailable for this capacity")};
    HdsBuildManifest manifest{std::string{build_manifest_schema_version}, size_bytes, {}};
    std::vector<std::vector<detail::PreparedRecord>> records;
    for (const auto &geometry : selected->partitions) {
        manifest.partitions.push_back({"PARTITION " + std::to_string(geometry.index + 1U), {}});
        auto prepared = detail::prepare_partition_records(manifest.partitions.back(), geometry, count, cancellation);
        if (!prepared)
            return std::unexpected{prepared.error()};
        records.push_back(std::move(*prepared));
    }
    // Capture sectors, not an image-sized buffer. Later writes overlay explicit zero ranges.
    std::map<std::uint64_t, std::array<std::byte, 512>> sectors;
    const auto capture = [&](std::uint64_t offset, std::span<const std::byte> bytes) -> Result<void> {
        if (offset > size_bytes || bytes.size() > size_bytes - offset)
            return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                              "SFS initialization exceeds target bounds")};
        while (!bytes.empty()) {
            const auto base = offset / 512U * 512U;
            const auto within = static_cast<std::size_t>(offset - base);
            const auto length = std::min(bytes.size(), 512U - within);
            std::memcpy(sectors[base].data() + within, bytes.data(), length);
            bytes = bytes.subspan(length);
            offset += length;
        }
        return {};
    };
    auto emitted = detail::emit_hds_records(manifest, selected->partitions, records, {}, capture, cancellation);
    if (!emitted)
        return std::unexpected{emitted.error()};
    HdsInitialization result{size_bytes, selected->partitions, {}};
    for (auto &[offset, bytes] : sectors) {
        if (result.writes.empty() || result.writes.back().offset + result.writes.back().bytes.size() != offset)
            result.writes.push_back({offset, {}});
        auto &output = result.writes.back().bytes;
        output.insert(output.end(), bytes.begin(), bytes.end());
    }
    return result;
}
} // namespace axk
