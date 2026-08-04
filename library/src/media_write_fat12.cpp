#include "axklib/writer_internal.hpp"

#include "axklib/file_publication.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <span>

// clang-format off
#include <ff.h>
#include <diskio.h>
// clang-format on

namespace {

constexpr std::size_t sector_size = 512;
constexpr std::size_t sector_count = 2880;
constexpr std::size_t sectors_per_fat = 9;
constexpr std::size_t first_fat_offset = sector_size;
constexpr std::size_t second_fat_offset = first_fat_offset + sectors_per_fat * sector_size;
constexpr std::byte floppy_media_descriptor{0xf0};
constexpr std::size_t boot_jump_offset = 0x00;
constexpr std::size_t oem_name_offset = 0x03;
constexpr std::size_t media_descriptor_offset = 0x15;
constexpr std::size_t sectors_per_track_offset = 0x18;
constexpr std::size_t head_count_offset = 0x1a;
constexpr std::size_t drive_number_offset = 0x24;
constexpr std::size_t reserved_boot_offset = 0x25;
constexpr std::size_t extended_boot_signature_offset = 0x26;
constexpr std::size_t volume_label_offset = 0x2b;
constexpr std::size_t filesystem_name_offset = 0x36;

struct FatDisk {
    std::vector<std::byte> bytes;
};

FatDisk *active_disk{};
std::mutex fatfs_mutex;

std::string fat_stem(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '_')
            result.push_back(static_cast<char>(std::toupper(byte)));
        if (result.size() == 8U)
            break;
    }
    return result.empty() ? "OBJECT" : result;
}

axk::Error fatfs_error(std::string message, FRESULT result) {
    return axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                           std::move(message) + " (FatFs result " + std::to_string(static_cast<unsigned int>(result)) +
                               ")");
}

void apply_yamaha_floppy_profile(std::span<std::byte> bytes) {
    constexpr std::array<std::byte, 3> jump{std::byte{0xeb}, std::byte{0x58}, std::byte{0x90}};
    constexpr std::string_view oem_name{"WINIMAGE"};
    constexpr std::string_view filesystem_name{"FAT12   "};

    std::ranges::copy(jump, bytes.begin() + boot_jump_offset);
    std::ranges::transform(oem_name, bytes.begin() + oem_name_offset,
                           [](char character) { return static_cast<std::byte>(character); });
    bytes[media_descriptor_offset] = floppy_media_descriptor;
    bytes[sectors_per_track_offset] = std::byte{18};
    bytes[sectors_per_track_offset + 1U] = std::byte{0};
    bytes[head_count_offset] = std::byte{2};
    bytes[head_count_offset + 1U] = std::byte{0};
    bytes[drive_number_offset] = std::byte{0};
    bytes[reserved_boot_offset] = std::byte{0};
    bytes[extended_boot_signature_offset] = std::byte{0x29};
    std::fill_n(bytes.begin() + volume_label_offset, 11, std::byte{' '});
    std::ranges::transform(filesystem_name, bytes.begin() + filesystem_name_offset,
                           [](char character) { return static_cast<std::byte>(character); });
    bytes[first_fat_offset] = floppy_media_descriptor;
    bytes[second_fat_offset] = floppy_media_descriptor;
}

} // namespace

extern "C" DSTATUS disk_initialize(BYTE drive) { return drive == 0U && active_disk != nullptr ? 0U : STA_NOINIT; }

extern "C" DSTATUS disk_status(BYTE drive) { return drive == 0U && active_disk != nullptr ? 0U : STA_NOINIT; }

extern "C" DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
    if (drive != 0U || active_disk == nullptr || buffer == nullptr || count == 0U || sector > sector_count ||
        count > sector_count - sector) {
        return RES_PARERR;
    }
    const auto offset = static_cast<std::size_t>(sector) * sector_size;
    std::memcpy(buffer, active_disk->bytes.data() + offset, static_cast<std::size_t>(count) * sector_size);
    return RES_OK;
}

extern "C" DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
    if (drive != 0U || active_disk == nullptr || buffer == nullptr || count == 0U || sector > sector_count ||
        count > sector_count - sector) {
        return RES_PARERR;
    }
    const auto offset = static_cast<std::size_t>(sector) * sector_size;
    std::memcpy(active_disk->bytes.data() + offset, buffer, static_cast<std::size_t>(count) * sector_size);
    return RES_OK;
}

extern "C" DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer) {
    if (drive != 0U || active_disk == nullptr)
        return RES_PARERR;
    switch (command) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *static_cast<LBA_t *>(buffer) = sector_count;
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

namespace axk::detail {

Result<std::vector<std::string>> plan_fat12_object_filenames(const PreparedMediaImage &image) {
    std::set<std::string> filenames{"YAMAHA.SYM"};
    for (const auto &retained : image.retained_files) {
        if (is_yamaha_floppy_catalog_path(retained.path))
            continue;
        if (retained.path.empty() || retained.path.size() > 12U ||
            retained.path.find_first_of("/\\") != std::string::npos || !filenames.insert(retained.path).second) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "retained FAT12 files must be unique root-level 8.3 names")};
        }
    }

    std::vector<std::string> result;
    result.reserve(image.objects.size());
    for (std::size_t index = 0U; index < image.objects.size(); ++index) {
        const auto &object = image.objects[index];
        if (!object.fat_filename.empty()) {
            if (object.fat_filename.size() > 12U || object.fat_filename.find_first_of("/\\") != std::string::npos ||
                !filenames.insert(object.fat_filename).second) {
                return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                                  "prepared FAT12 object filenames must be unique 8.3 names")};
            }
            result.push_back(object.fat_filename);
            continue;
        }
        if (index >= 222U) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "Yamaha floppy catalogs support at most 222 generated objects")};
        }
        auto stem = fat_stem(object.name);
        const auto slot = static_cast<std::uint16_t>(index + 2U);
        auto filename = *yamaha_floppy_physical_filename(stem, slot);
        for (std::size_t disambiguator = 1U; filenames.contains(filename) && disambiguator < 100U; ++disambiguator) {
            const auto suffix = std::to_string(disambiguator);
            stem.resize(std::min<std::size_t>(stem.size(), 8U - suffix.size()));
            filename = *yamaha_floppy_physical_filename(stem + suffix, slot);
        }
        if (!filenames.insert(filename).second) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "FAT12 object filename space is exhausted")};
        }
        result.push_back(std::move(filename));
    }
    return result;
}

Result<std::vector<std::byte>> build_fat12_image(const PreparedMediaImage &image,
                                                 const CancellationToken &cancellation) {
    const auto retained_count = static_cast<std::size_t>(std::ranges::count_if(
        image.retained_files, [](const PreparedMediaFile &file) { return !is_yamaha_floppy_catalog_path(file.path); }));
    if (image.objects.size() + retained_count + 1U > 224U) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "FAT12 floppy profile supports at "
                                          "most 224 root-directory objects")};
    }
    auto object_filenames = plan_fat12_object_filenames(image);
    if (!object_filenames)
        return std::unexpected{object_filenames.error()};
    YamahaFloppyCatalog catalog;
    if (image.floppy_catalog) {
        catalog = *image.floppy_catalog;
    } else {
        for (const auto &retained : image.retained_files) {
            if (!is_yamaha_floppy_catalog_path(retained.path))
                continue;
            if (auto retained_catalog = decode_yamaha_floppy_catalog(retained.payload); retained_catalog)
                catalog.disk_name = std::move(retained_catalog->disk_name);
            break;
        }
        if (catalog.disk_name.empty()) {
            auto disk_name = yamaha_floppy_disk_name(image.manifest.volume_name, 1U);
            if (!disk_name)
                return std::unexpected{disk_name.error()};
            catalog.disk_name = std::move(*disk_name);
        }
        catalog.categories = yamaha_floppy_categories(image.objects);
        catalog.files.reserve(image.objects.size());
        for (std::size_t index = 0U; index < image.objects.size(); ++index) {
            const auto slot = yamaha_floppy_filename_slot((*object_filenames)[index]);
            if (!slot) {
                return std::unexpected{slot.error()};
            }
            auto path = yamaha_floppy_object_path(image.objects[index].type, image.objects[index].name);
            if (!path)
                return std::unexpected{path.error()};
            catalog.files.push_back({*slot, std::move(*path)});
        }
    }
    auto catalog_bytes = encode_yamaha_floppy_catalog(catalog.disk_name, catalog.files, catalog.categories);
    if (!catalog_bytes)
        return std::unexpected{catalog_bytes.error()};

    std::scoped_lock lock{fatfs_mutex};
    FatDisk disk{std::vector<std::byte>(sector_count * sector_size)};
    active_disk = &disk;
    const auto reset_disk = [&]() { active_disk = nullptr; };
    std::array<std::byte, 4096> work{};
    const MKFS_PARM options{static_cast<BYTE>(FM_FAT | FM_SFD), 2, 0, 224, sector_size};
    auto status = f_mkfs("", &options, work.data(), static_cast<UINT>(work.size()));
    if (status != FR_OK) {
        reset_disk();
        return std::unexpected{fatfs_error("could not format FAT12 image", status)};
    }
    apply_yamaha_floppy_profile(disk.bytes);
    FATFS filesystem{};
    status = f_mount(&filesystem, "", 1);
    if (status != FR_OK) {
        reset_disk();
        return std::unexpected{fatfs_error("could not mount formatted FAT12 image", status)};
    }

    const auto write_file = [&](std::string_view filename, std::span<const std::byte> payload) -> Result<void> {
        FIL file{};
        auto write_status = f_open(&file, std::string{filename}.c_str(), FA_CREATE_NEW | FA_WRITE);
        if (write_status == FR_OK) {
            UINT written{};
            write_status = f_write(&file, payload.data(), static_cast<UINT>(payload.size()), &written);
            if (write_status == FR_OK && written != payload.size())
                write_status = FR_DISK_ERR;
            const auto close_status = f_close(&file);
            if (write_status == FR_OK)
                write_status = close_status;
        }
        if (write_status != FR_OK)
            return std::unexpected{fatfs_error("could not write FAT12 file", write_status)};
        return {};
    };
    enum class FileSource : std::uint8_t { retained, object };
    struct PlannedFile {
        std::string path;
        FileSource source{};
        std::size_t index{};
    };
    std::vector<PlannedFile> files;
    files.reserve(retained_count + image.objects.size());
    for (std::size_t index = 0U; index < image.retained_files.size(); ++index) {
        if (!is_yamaha_floppy_catalog_path(image.retained_files[index].path))
            files.push_back({image.retained_files[index].path, FileSource::retained, index});
    }
    for (std::size_t index = 0U; index < image.objects.size(); ++index)
        files.push_back({(*object_filenames)[index], FileSource::object, index});
    std::ranges::sort(files, {}, &PlannedFile::path);

    for (const auto &file : files) {
        if (const auto check = cancellation.check(); !check) {
            f_mount(nullptr, "", 0);
            reset_disk();
            return std::unexpected{check.error()};
        }
        if (file.source == FileSource::retained) {
            if (auto written = write_file(file.path, image.retained_files[file.index].payload); !written) {
                f_mount(nullptr, "", 0);
                reset_disk();
                return std::unexpected{written.error()};
            }
            continue;
        }
        const auto &object = image.objects[file.index];
        if (object.payload == nullptr || object.size() > std::numeric_limits<UINT>::max()) {
            f_mount(nullptr, "", 0);
            reset_disk();
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "FAT12 object payload reader is missing or too large")};
        }
        std::vector<std::byte> payload(static_cast<std::size_t>(object.size()));
        if (auto read = object.payload->read_exact_at(0U, payload); !read) {
            f_mount(nullptr, "", 0);
            reset_disk();
            return std::unexpected{read.error()};
        }
        if (auto written = write_file(file.path, payload); !written) {
            f_mount(nullptr, "", 0);
            reset_disk();
            return std::unexpected{written.error()};
        }
    }
    if (auto written = write_file("YAMAHA.SYM", *catalog_bytes); !written) {
        f_mount(nullptr, "", 0);
        reset_disk();
        return std::unexpected{written.error()};
    }
    f_mount(nullptr, "", 0);
    reset_disk();

    return disk.bytes;
}

Result<void> write_fat12_image(const PreparedMediaImage &image, TemporaryPublication &publication,
                               const CancellationToken &cancellation) {
    auto bytes = build_fat12_image(image, cancellation);
    if (!bytes)
        return std::unexpected{bytes.error()};
    if (auto resized = publication.resize(bytes->size()); !resized)
        return resized;
    return publication.write_at(0U, *bytes);
}

} // namespace axk::detail
