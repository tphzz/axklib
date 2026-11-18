#include "axklib/filesystems/fat/floppy_writer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// clang-format off
#include <ff.h>
#include <diskio.h>
// clang-format on

namespace {
constexpr std::size_t sector_size = 512;
std::span<std::byte> active_disk;
std::mutex fatfs_mutex;

bool valid_request(BYTE drive, LBA_t sector, UINT count) {
    const auto sectors = active_disk.size() / sector_size;
    return drive == 0 && count != 0 && sector < sectors && count <= sectors - sector;
}
} // namespace

extern "C" DSTATUS disk_initialize(BYTE drive) { return drive == 0 && !active_disk.empty() ? 0 : STA_NOINIT; }
extern "C" DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }

extern "C" DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
    if (buffer == nullptr || !valid_request(drive, sector, count))
        return RES_PARERR;
    std::memcpy(buffer, active_disk.data() + static_cast<std::size_t>(sector) * sector_size,
                static_cast<std::size_t>(count) * sector_size);
    return RES_OK;
}

extern "C" DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
    if (buffer == nullptr || !valid_request(drive, sector, count))
        return RES_PARERR;
    std::memcpy(active_disk.data() + static_cast<std::size_t>(sector) * sector_size, buffer,
                static_cast<std::size_t>(count) * sector_size);
    return RES_OK;
}

extern "C" DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer) {
    if (disk_status(drive) != 0 || (command != CTRL_SYNC && buffer == nullptr))
        return RES_PARERR;
    switch (command) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *static_cast<LBA_t *>(buffer) = static_cast<LBA_t>(active_disk.size() / sector_size);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *static_cast<WORD *>(buffer) = sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *static_cast<DWORD *>(buffer) = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

namespace axk::filesystems {
namespace {

struct Geometry {
    std::uint16_t sectors;
    std::uint16_t root_entries;
    std::uint8_t sectors_per_cluster;
    std::uint8_t sectors_per_fat;
    std::uint8_t sectors_per_track;
    std::byte media;

    std::size_t root_offset() const { return (1U + 2U * sectors_per_fat) * sector_size; }
    std::size_t cluster_bytes() const { return sectors_per_cluster * sector_size; }
    std::size_t disk_bytes() const { return sectors * sector_size; }
    std::size_t data_clusters() const { return (disk_bytes() - root_offset() - 32U * root_entries) / cluster_bytes(); }
};

Result<Geometry> geometry(FloppyLayout layout) {
    switch (layout) {
    case FloppyLayout::dd_720k:
        return Geometry{1440, 112, 2, 3, 9, std::byte{0xf9}};
    case FloppyLayout::hd_1440k:
        return Geometry{2880, 224, 1, 9, 18, std::byte{0xf0}};
    }
    return std::unexpected(
        make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, "Unknown FAT12 layout"));
}

Result<void> check_files(const Geometry &g, std::span<const FloppyFile> files) {
    if (files.size() > g.root_entries)
        return std::unexpected(
            make_error(ErrorCode::unsupported_profile, ErrorCategory::allocation,
                       std::format("FAT12 needs {} root entries; {} available", files.size(), g.root_entries)));
    auto remaining = g.data_clusters();
    for (const auto &file : files) {
        // FatFs checks the short filename itself. Do not let it interpret a drive, path or terminator.
        if (file.name.empty() || file.name.size() > 12 || file.name.front() == '.' || file.name.back() == '.' ||
            file.name.find_first_of("/\\:") != std::string::npos ||
            std::ranges::any_of(file.name, [](unsigned char c) { return c <= 0x20; }))
            return std::unexpected(
                make_error(ErrorCode::invalid_argument, ErrorCategory::manifest, "Expected a FAT root filename"));
        const auto *memory = std::get_if<std::span<const std::byte>>(&file.payload);
        const auto size = memory
                              ? memory->size()
                              : std::get<std::reference_wrapper<const RandomAccessReader>>(file.payload).get().size();
        const auto clusters = size / g.cluster_bytes() + (size % g.cluster_bytes() != 0U);
        if (clusters > remaining)
            return std::unexpected(make_error(
                ErrorCode::unsupported_profile, ErrorCategory::allocation,
                std::format("FAT12 file '{}' needs {} clusters; {} remain", file.name, clusters, remaining)));
        remaining -= static_cast<std::size_t>(clusters);
    }
    return {};
}

Error fatfs_error(std::string_view operation, FRESULT status) {
    return make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                      std::format("{} (FatFs result {})", operation, static_cast<unsigned int>(status)));
}

class DiskMount {
  public:
    explicit DiskMount(std::span<std::byte> disk) { active_disk = disk; }
    DiskMount(const DiskMount &) = delete;
    DiskMount &operator=(const DiskMount &) = delete;
    ~DiskMount() {
        f_mount(nullptr, "", 0);
        active_disk = {};
    }
    FATFS filesystem{};
};

Result<void> write_payload(FIL &dest, const FloppyFile &file, const CancellationToken &cancellation) {
    const auto write = [&](std::span<const std::byte> bytes) -> Result<void> {
        if (!bytes.empty()) {
            UINT written{};
            const auto status = f_write(&dest, bytes.data(), static_cast<UINT>(bytes.size()), &written);
            if (status != FR_OK || written != bytes.size())
                return std::unexpected(fatfs_error("Could not write the full FAT12 payload", status));
        }
        return cancellation.check();
    };
    if (const auto *memory = std::get_if<std::span<const std::byte>>(&file.payload))
        return write(*memory);
    const auto &reader = std::get<std::reference_wrapper<const RandomAccessReader>>(file.payload).get();
    const auto size = reader.size();
    std::array<std::byte, 16384> buffer;
    for (std::uint64_t offset = 0; offset < size;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size - offset, buffer.size()));
        const auto chunk = std::span{buffer}.first(count);
        if (const auto read = reader.read_exact_at(offset, chunk); !read)
            return std::unexpected(read.error());
        if (const auto result = write(chunk); !result)
            return result;
        offset += count;
    }
    return {};
}

Result<void> populate(std::span<std::byte> disk, const Geometry &g, std::span<const FloppyFile> files,
                      const CancellationToken &cancellation) {
    std::scoped_lock lock{fatfs_mutex};
    DiskMount mount{disk};
    std::array<std::byte, 4096> scratch{};
    const MKFS_PARM options{static_cast<BYTE>(FM_FAT | FM_SFD), 2, 0, g.root_entries,
                            static_cast<DWORD>(g.cluster_bytes())};
    if (const auto status = f_mkfs("", &options, scratch.data(), static_cast<UINT>(scratch.size())); status != FR_OK)
        return std::unexpected(fatfs_error("Could not format FAT12 image", status));
    if (const auto status = f_mount(&mount.filesystem, "", 1); status != FR_OK)
        return std::unexpected(fatfs_error("Could not mount FAT12 image", status));
    const auto &fs = mount.filesystem;
    if (fs.fs_type != FS_FAT12 || fs.n_fats != 2 || fs.fatbase != 1 || fs.fsize != g.sectors_per_fat ||
        fs.n_rootdir != g.root_entries || fs.csize != g.sectors_per_cluster || fs.n_fatent != g.data_clusters() + 2)
        return std::unexpected(make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "FatFs did not create the requested floppy geometry"));
    for (const auto &file : files) {
        if (const auto check = cancellation.check(); !check)
            return check;
        FIL dest{};
        if (const auto status = f_open(&dest, file.name.c_str(), FA_CREATE_NEW | FA_WRITE); status != FR_OK)
            return std::unexpected(fatfs_error("Could not create FAT12 file " + file.name, status));
        const auto result = write_payload(dest, file, cancellation);
        const auto closed = f_close(&dest);
        if (!result)
            return result;
        if (closed != FR_OK)
            return std::unexpected(fatfs_error("Could not close FAT12 file", closed));
    }
    return cancellation.check();
}
} // namespace

Result<FloppyCapacity> floppy_capacity(FloppyLayout layout) {
    const auto g = geometry(layout);
    if (!g)
        return std::unexpected(g.error());
    return FloppyCapacity{g->disk_bytes(), g->cluster_bytes(), g->data_clusters(), g->root_entries};
}

Result<std::vector<std::byte>> build_floppy(FloppyLayout layout, std::span<const FloppyFile> files,
                                            const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected(check.error());
    const auto g = geometry(layout);
    if (!g)
        return std::unexpected(g.error());
    if (const auto valid = check_files(*g, files); !valid)
        return std::unexpected(valid.error());
    std::vector<std::byte> disk(g->disk_bytes());
    if (const auto result = populate(disk, *g, files, cancellation); !result)
        return std::unexpected(result.error());

    // FatFs formats fixed media. Set the removable geometry and both copies of the FAT media marker.
    disk[0x15] = disk[sector_size] = disk[(1U + g->sectors_per_fat) * sector_size] = g->media;
    disk[0x18] = static_cast<std::byte>(g->sectors_per_track);
    disk[0x19] = disk[0x1b] = disk[0x24] = std::byte{};
    disk[0x1a] = std::byte{2};
    for (std::size_t i = 0; i < files.size(); ++i)
        disk[g->root_offset() + 32U * i + 11U] = std::byte{};
    return disk;
}
} // namespace axk::filesystems
