#include "writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>

// clang-format off
#include <ff.h>
#include <diskio.h>
// clang-format on

namespace {

constexpr std::size_t sector_size = 512;
constexpr std::size_t sector_count = 2880;

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
                         std::move(message) + " (FatFs result " +
                             std::to_string(static_cast<unsigned int>(result)) + ")");
}

} // namespace

extern "C" DSTATUS disk_initialize(BYTE drive) {
  return drive == 0U && active_disk != nullptr ? 0U : STA_NOINIT;
}

extern "C" DSTATUS disk_status(BYTE drive) {
  return drive == 0U && active_disk != nullptr ? 0U : STA_NOINIT;
}

extern "C" DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
  if (drive != 0U || active_disk == nullptr || buffer == nullptr || count == 0U ||
      sector > sector_count || count > sector_count - sector) {
    return RES_PARERR;
  }
  const auto offset = static_cast<std::size_t>(sector) * sector_size;
  std::memcpy(buffer, active_disk->bytes.data() + offset,
              static_cast<std::size_t>(count) * sector_size);
  return RES_OK;
}

extern "C" DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
  if (drive != 0U || active_disk == nullptr || buffer == nullptr || count == 0U ||
      sector > sector_count || count > sector_count - sector) {
    return RES_PARERR;
  }
  const auto offset = static_cast<std::size_t>(sector) * sector_size;
  std::memcpy(active_disk->bytes.data() + offset, buffer,
              static_cast<std::size_t>(count) * sector_size);
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

Result<void> write_fat12_image(const PreparedMediaImage &image,
                               const std::filesystem::path &temporary_path,
                               const CancellationToken &cancellation) {
  if (image.objects.size() > 224U) {
    return std::unexpected{
        make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                   "FAT12 floppy profile supports at most 224 root-directory objects")};
  }
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
  FATFS filesystem{};
  status = f_mount(&filesystem, "", 1);
  if (status != FR_OK) {
    reset_disk();
    return std::unexpected{fatfs_error("could not mount formatted FAT12 image", status)};
  }

  std::set<std::string> stems;
  for (std::size_t index = 0; index < image.objects.size(); ++index) {
    if (const auto check = cancellation.check(); !check) {
      f_mount(nullptr, "", 0);
      reset_disk();
      return std::unexpected{check.error()};
    }
    auto stem = fat_stem(image.objects[index].name);
    if (!stems.insert(stem).second) {
      const auto suffix = std::to_string(index + 1U);
      stem.resize(
          std::min<std::size_t>(stem.size(), 8U - std::min<std::size_t>(suffix.size(), 7U)));
      stem += suffix;
      if (!stems.insert(stem).second) {
        f_mount(nullptr, "", 0);
        reset_disk();
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "FAT12 object names cannot be made unique")};
      }
    }
    const auto filename =
        stem + "." + std::to_string(1000U + static_cast<unsigned int>(index + 1U)).substr(1);
    FIL file{};
    status = f_open(&file, filename.c_str(), FA_CREATE_NEW | FA_WRITE);
    if (status == FR_OK) {
      UINT written{};
      const auto &payload = image.objects[index].payload;
      status = f_write(&file, payload.data(), static_cast<UINT>(payload.size()), &written);
      if (status == FR_OK && written != payload.size())
        status = FR_DISK_ERR;
      const auto close_status = f_close(&file);
      if (status == FR_OK)
        status = close_status;
    }
    if (status != FR_OK) {
      f_mount(nullptr, "", 0);
      reset_disk();
      return std::unexpected{fatfs_error("could not write FAT12 object file", status)};
    }
  }
  f_mount(nullptr, "", 0);
  reset_disk();

  std::ofstream output{temporary_path, std::ios::binary | std::ios::trunc};
  if (!output) {
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not create temporary FAT12 image")};
  }
  output.write(reinterpret_cast<const char *>(disk.bytes.data()),
               static_cast<std::streamsize>(disk.bytes.size()));
  if (!output) {
    return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                      "could not write temporary FAT12 image")};
  }
  return {};
}

} // namespace axk::detail
