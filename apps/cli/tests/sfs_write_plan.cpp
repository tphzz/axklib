#include "sfs_write_plan.hpp"

#include "axklib/file_publication.hpp"
#include "axklib/io.hpp"
#include "axklib/sfs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace axk::test::sfs_write {
namespace {
constexpr std::uint64_t sector_size = 512U;
constexpr std::size_t maximum_guard_bytes = 64U * 1024U * 1024U - 4096U;
using Range = std::pair<std::uint64_t, std::uint64_t>;

auto refuse(std::string message) {
    return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message))};
}

// Freeze every sector observed by the canonical parser. Later reads reuse that snapshot, and
// guards are reread from the backup before publication, so allocation facts and guard bytes agree.
class SnapshotReader final : public RandomAccessReader {
  public:
    explicit SnapshotReader(std::shared_ptr<FileReader> file) : file_(std::move(file)) {}
    std::uint64_t size() const noexcept override { return file_->size(); }
    Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (offset > size() || destination.size() > size() - offset)
            return refuse("metadata read exceeds the backup");
        if (destination.empty())
            return {};
        const auto begin = offset / sector_size;
        const auto end = (offset + destination.size() + sector_size - 1U) / sector_size;
        const auto count = end - begin;
        if (count > maximum_guard_bytes / sector_size)
            return refuse("metadata read exceeds the snapshot limit");
        bool missing = false;
        for (auto lba = begin; lba < end; ++lba)
            missing |= !sectors_.contains(lba);
        if (missing) {
            std::vector<std::byte> bytes(static_cast<std::size_t>(count * sector_size));
            if (const auto read = file_->read_exact_at(begin * sector_size, bytes); !read)
                return read;
            for (auto lba = begin; lba < end; ++lba) {
                const auto part =
                    std::span{bytes}.subspan(static_cast<std::size_t>((lba - begin) * sector_size), sector_size);
                if (const auto it = sectors_.find(lba); it != sectors_.end()) {
                    if (!std::ranges::equal(it->second, part))
                        return refuse("backup changed while parsing metadata");
                } else {
                    if (sectors_.size() >= maximum_guard_bytes / sector_size)
                        return refuse("metadata snapshots exceed 64 MiB");
                    std::array<std::byte, sector_size> sector{};
                    std::ranges::copy(part, sector.begin());
                    sectors_.emplace(lba, sector);
                }
            }
        }
        std::size_t copied = 0;
        while (copied < destination.size()) {
            const auto absolute = offset + copied;
            const auto within = static_cast<std::size_t>(absolute % sector_size);
            const auto amount = std::min(destination.size() - copied, static_cast<std::size_t>(sector_size) - within);
            const auto &sector = sectors_.at(absolute / sector_size);
            std::copy_n(sector.begin() + static_cast<std::ptrdiff_t>(within), amount,
                        destination.begin() + static_cast<std::ptrdiff_t>(copied));
            copied += amount;
        }
        return {};
    }
    Result<std::vector<Guard>> guards() const {
        std::vector<Guard> result;
        for (const auto &[lba, bytes] : sectors_) {
            if (result.empty() || result.back().lba + result.back().bytes.size() / sector_size != lba)
                result.push_back({lba, {}});
            auto &out = result.back().bytes;
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        if (result.size() > 4092U)
            return refuse("too many separate metadata guards");
        for (const auto &guard : result) {
            std::vector<std::byte> current(guard.bytes.size());
            if (const auto read = file_->read_exact_at(guard.lba * sector_size, current); !read)
                return std::unexpected{read.error()};
            if (current != guard.bytes)
                return refuse("backup changed after allocation analysis");
        }
        return result;
    }

  private:
    std::shared_ptr<FileReader> file_;
    mutable std::map<std::uint64_t, std::array<std::byte, sector_size>> sectors_;
};

std::vector<Range> merge(std::vector<Range> ranges) {
    std::ranges::sort(ranges);
    std::vector<Range> result;
    for (const auto &[begin, end] : ranges) {
        if (begin >= end)
            continue;
        if (result.empty() || result.back().second < begin)
            result.emplace_back(begin, end);
        else
            result.back().second = std::max(result.back().second, end);
    }
    return result;
}

bool empty_sector(std::span<const std::byte> bytes) {
    return (bytes.front() == std::byte{0} || bytes.front() == std::byte{0xff}) &&
           std::ranges::all_of(bytes, [&](std::byte byte) { return byte == bytes.front(); });
}

Result<void> select_empty_sector(const FileReader &file, Plan &plan) {
    const auto center = plan.free_begin + (plan.free_end - plan.free_begin) / 2U;
    plan.lba = center;
    plan.original.resize(sector_size);
    if (const auto read = file.read_exact_at(center * sector_size, plan.original); !read)
        return read;
    if (empty_sector(plan.original))
        return {};

    // Allocation and metadata margins are already proven. Search only this bounded
    // window for an empty sector, preferring the closest to the range's midpoint.
    const auto begin = center - std::min(center - plan.free_begin, maximum_search_sectors / 2U);
    const auto end = begin + std::min(maximum_search_sectors, plan.free_end - begin);
    constexpr std::uint64_t batch_sectors = 128U;
    std::vector<std::byte> buffer(batch_sectors * sector_size);
    auto closest = std::numeric_limits<std::uint64_t>::max();
    for (auto cursor = begin; cursor < end;) {
        const auto count = std::min(batch_sectors, end - cursor);
        auto bytes = std::span{buffer}.first(static_cast<std::size_t>(count * sector_size));
        if (const auto read = file.read_exact_at(cursor * sector_size, bytes); !read)
            return read;
        for (std::uint64_t i = 0U; i < count; ++i) {
            const auto lba = cursor + i;
            const auto distance = lba < center ? center - lba : lba - center;
            const auto sector = bytes.subspan(static_cast<std::size_t>(i * sector_size), sector_size);
            if (distance >= closest || !empty_sector(sector))
                continue;
            closest = distance;
            plan.lba = lba;
            std::ranges::copy(sector, plan.original.begin());
        }
        cursor += count;
    }
    if (closest == std::numeric_limits<std::uint64_t>::max())
        return refuse(std::format("no uniformly 00 or FF sector in the validated free-space search window [{}, {}); "
                                  "search is limited to 64 MiB and no write plan was created",
                                  begin, end));
    return {};
}

Result<Plan> prepare_file(const std::shared_ptr<FileReader> &file, const std::filesystem::path &path) {
    if (file->size() == 0U || file->size() % sector_size != 0U)
        return refuse("a complete 512-byte-sector target backup is required");
    const auto snapshot = std::make_shared<SnapshotReader>(file);
    const auto container = open_image(snapshot, path);
    if (!container)
        return std::unexpected{container.error()};
    const auto total = file->size() / sector_size;
    if (container->superblock().sector_size_bytes != sector_size ||
        container->superblock().total_sector_count != total || !container->backup_superblock_matches() ||
        !container->diagnostics().empty() || container->partitions().empty())
        return refuse("SFS geometry, superblock copies or container diagnostics are not safe for a write check");
    std::vector<Range> partitions;
    std::vector<Range> blocked;
    std::vector<Range> metadata{{0U, 2U}};
    for (const auto &partition : container->partitions()) {
        if (!partition.backup_header_matches || !partition.diagnostics.empty() ||
            !allocation_is_safe_for_mutation(partition.allocation) || !partition.allocation.free_space)
            return refuse("partition metadata or reconstructed allocation is not safe for a write check");
        const std::uint64_t start = partition.start_sector;
        const std::uint64_t end = start + partition.sector_count;
        const std::uint64_t spc = partition.sectors_per_cluster;
        const auto data_end = start + static_cast<std::uint64_t>(partition.cluster_count) * spc;
        if (start < 2U || end > total || data_end > end)
            return refuse("invalid partition boundaries");
        partitions.emplace_back(start, end);
        blocked.emplace_back(data_end, end);
        const auto reserved_end =
            start + static_cast<std::uint64_t>(partition.allocation.free_space->reserved_cluster_count) * spc;
        metadata.emplace_back(start, reserved_end);
        for (const auto &used : partition.allocation.bitmap_copy1.used_cluster_ranges)
            blocked.emplace_back(start + static_cast<std::uint64_t>(used.start_cluster) * spc,
                                 start + static_cast<std::uint64_t>(used.end_cluster) * spc);
        for (const auto &record : partition.records) {
            for (const auto cluster : record.continuation_clusters) {
                const auto lba = start + static_cast<std::uint64_t>(cluster) * spc;
                metadata.emplace_back(lba, lba + spc);
            }
            if (record.payload_kind == PayloadKind::directory)
                for (const auto &extent : record.extents) {
                    const auto lba = start + static_cast<std::uint64_t>(extent.cluster_offset) * spc;
                    metadata.emplace_back(lba, lba + static_cast<std::uint64_t>(extent.cluster_count) * spc);
                }
        }
    }
    std::ranges::sort(partitions);
    for (std::size_t i = 1U; i < partitions.size(); ++i)
        if (partitions[i - 1U].second > partitions[i].first)
            return refuse("overlapping SFS partitions");
    metadata = merge(std::move(metadata));
    for (const auto &[begin, end] : metadata) {
        if (end > total || end - begin > maximum_guard_bytes / sector_size)
            return refuse("metadata geometry exceeds the guard limit");
        std::vector<std::byte> bytes(static_cast<std::size_t>((end - begin) * sector_size));
        if (const auto read = snapshot->read_exact_at(begin * sector_size, bytes); !read)
            return std::unexpected{read.error()};
        blocked.emplace_back(begin > exclusion_sectors ? begin - exclusion_sectors : 0U,
                             end + std::min(exclusion_sectors, total - end));
    }
    blocked.emplace_back(0U, std::min(exclusion_sectors, total));
    blocked.emplace_back(total - std::min(exclusion_sectors, total), total);
    blocked = merge(std::move(blocked));
    Range largest{};
    const auto consider = [&](std::uint64_t begin, std::uint64_t end) {
        if (end > begin && end - begin > largest.second - largest.first)
            largest = {begin, end};
    };
    for (const auto &[begin, end] : partitions) {
        auto cursor = begin;
        for (const auto &[used_begin, used_end] : blocked) {
            if (used_end <= cursor || used_begin >= end)
                continue;
            consider(cursor, std::min(used_begin, end));
            cursor = std::max(cursor, std::min(used_end, end));
        }
        consider(cursor, end);
    }
    if (largest.first == largest.second)
        return refuse("no free sector satisfies the 16 MiB metadata/target-edge exclusion margin");
    Plan plan;
    plan.sector_count = total;
    plan.free_begin = largest.first;
    plan.free_end = largest.second;
    if (const auto selected = select_empty_sector(*file, plan); !selected)
        return std::unexpected{selected.error()};
    auto guards = snapshot->guards();
    if (!guards)
        return std::unexpected{guards.error()};
    plan.guards = std::move(*guards);
    for (const auto &guard : plan.guards)
        if (plan.lba >= guard.lba && plan.lba - guard.lba < guard.bytes.size() / sector_size)
            return refuse("candidate overlaps a parser-observed metadata guard");
    return plan;
}
} // namespace

Result<Plan> prepare(const std::filesystem::path &backup) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(backup, error) || error)
        return refuse("planner input must be a regular backup file, never a physical device");
    auto file = FileReader::open(backup);
    if (!file)
        return std::unexpected{file.error()};
    return prepare_file(*file, backup);
}

Result<void> save(const std::filesystem::path &path, const Plan &plan) {
    // Serialize the standalone consumer's bounded RAWPLAN! binary contract.
    std::vector<std::byte> bytes;
    const auto number = [&](std::uint64_t value, std::size_t count) {
        for (std::size_t i = 0U; i < count; ++i)
            bytes.push_back(static_cast<std::byte>((value >> (8U * i)) & 255U));
    };
    for (const char c : std::string_view{"RAWPLAN!"})
        bytes.push_back(static_cast<std::byte>(c));
    number(sector_size, 4U);
    number(plan.sector_count, 8U);
    number(plan.lba, 8U);
    number(plan.guards.size(), 4U);
    bytes.insert(bytes.end(), plan.original.begin(), plan.original.end());
    for (const auto &guard : plan.guards) {
        number(guard.lba, 8U);
        number(guard.bytes.size(), 4U);
        bytes.insert(bytes.end(), guard.bytes.begin(), guard.bytes.end());
    }
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xedb88320U : 0U);
    }
    number(crc ^ 0xffffffffU, 4U);
    auto output = detail::TemporaryPublication::create(path);
    if (!output)
        return std::unexpected{output.error()};
    if (const auto append = output->append(bytes); !append)
        return append;
    const auto published = output->publish(detail::PublicationMode::create_only);
    if (!published)
        return std::unexpected{published.error()};
    if (published->durability != PublicationDurability::confirmed)
        return refuse("plan publication durability could not be confirmed");
    return {};
}
} // namespace axk::test::sfs_write
