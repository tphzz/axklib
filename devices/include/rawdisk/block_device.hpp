// rawdisk: sector-level access to USB disks (Windows / macOS / Linux) and disk images
// behind one interface. C++23.
//
// Error handling: no function in this header throws, except AlignedBuffer's constructor
// (std::bad_alloc). Failures are returned as std::error_code (see errors.hpp).
//
// Thread safety: a BlockDevice may be used from several threads; operations on one
// device are serialized internally. Different devices are independent.
#pragma once

#include "rawdisk/errors.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace rawdisk {

template <class T> using Result = std::expected<T, std::error_code>;

enum class Access { ReadOnly, ReadWrite };

// Buffer alignment that satisfies unbuffered/direct I/O on all three platforms.
// Unaligned buffers are accepted everywhere; they just cost an extra copy.
inline constexpr std::size_t kIoAlignment = 4096;

class BlockDevice {
  public:
    virtual ~BlockDevice() = default;
    BlockDevice(const BlockDevice &) = delete;
    BlockDevice &operator=(const BlockDevice &) = delete;

    virtual std::uint32_t sector_size() const noexcept = 0;
    virtual std::uint64_t sector_count() const noexcept = 0;
    virtual bool writable() const noexcept = 0;
    virtual bool uses_elevation_helper() const noexcept { return false; }
    std::uint64_t size_bytes() const noexcept { return std::uint64_t{sector_size()} * sector_count(); }

    // Transfers out.size() / sector_size() whole sectors starting at `lba`.
    // Errc::bad_length if the size is not a multiple of sector_size(),
    // Errc::out_of_range if the range passes the last sector.
    Result<void> read(std::uint64_t lba, std::span<std::byte> out) noexcept;
    Result<void> write(std::uint64_t lba, std::span<const std::byte> in) noexcept;

    // Pushes written data to the medium (OS cache and, where supported, the reader's cache).
    // Call before telling the user the card can be removed. No-op on read-only handles.
    Result<void> flush() noexcept;

  protected:
    BlockDevice() = default;
    // Called with validated, sector-aligned ranges, kIoAlignment-aligned buffers of at most
    // 1 MiB, serialized by the base class. Offsets are in bytes.
    virtual Result<void> read_raw(std::uint64_t offset, std::span<std::byte> out) = 0;
    virtual Result<void> write_raw(std::uint64_t offset, std::span<const std::byte> in) = 0;
    virtual Result<void> flush_raw() = 0;
    // Called once per write() with the whole validated range, before anything is written,
    // so a device can refuse a range atomically instead of failing halfway through.
    virtual Result<void> check_write(std::uint64_t /*lba*/, std::uint64_t /*sectors*/) { return {}; }

  private:
    std::mutex mutex_;
};

struct DeviceInfo {
    std::string path;   // \\.\PhysicalDrive2 | /dev/sdb | /dev/rdisk4
    std::string model;  // vendor + product as reported by the reader
    std::string serial; // USB serial number; empty if the reader has none
    std::uint64_t size_bytes = 0;
    std::uint32_t sector_size = 0;
    bool removable = false;
    std::string carrier_id{};    // OS USB parent identity; shared by all LUNs/slices
    std::string connection_id{}; // OS instance identity, not a card content hash

    friend bool operator==(const DeviceInfo &, const DeviceInfo &) = default;
};

// USB whole disks with media present (card inserted). Never needs elevated privileges.
Result<std::vector<DeviceInfo>> list_usb_devices() noexcept;

// Opens a device returned by list_usb_devices(). Before any data is touched it:
//  - re-enumerates and requires the same path, model, serial, size and sector size
//    (Errc::device_changed otherwise; drive numbers are reassigned on replug),
//  - verifies the identity again on the opened handle (closes the race with re-enumeration),
//  - reads the first and the last sector (Errc::capacity_unreadable if the reader
//    reports a capacity it cannot actually read).
// ReadWrite additionally fails with Errc::device_busy if a volume on the disk is mounted
// or in use (Windows: volumes on the card are locked and dismounted while it is open), and
// with Errc::write_protected for write-protected media.
//
// Privileges: Windows elevates on demand through a UAC prompt (see ElevationOptions);
// macOS uses authopen; Linux uses UDisks2/polkit on permission denial. POSIX authorization
// has a 120-second deadline. The application itself never needs to run as root.
Result<std::unique_ptr<BlockDevice>> open_device(const DeviceInfo &expected, Access access) noexcept;

// Raw disk image (regular files only; device paths are refused). The size must be a
// non-zero multiple of sector_size (512..65536, power of two). ReadWrite takes an exclusive
// lock and ReadOnly a shared lock (advisory on POSIX); conflicts give Errc::device_busy.
Result<std::unique_ptr<BlockDevice>> open_image(const std::filesystem::path &path, Access access,
                                                std::uint32_t sector_size = 512) noexcept;

// `allow` controls system authorization on every platform. Other fields are Windows only.
// When opening a device fails with "access denied" on Windows and the
// process is not elevated, rawdisk launches `helper_path` elevated (one UAC prompt) and does
// the raw I/O through it. One helper serves all devices opened while any of them (or a
// hold_privileges() token) stays alive; the helper exits when the last one is gone or the app exits.
struct ElevationOptions {
    bool allow = true;
    std::filesystem::path helper_path;         // empty: axklib_helper.exe next to the app's .exe
    void *parent_window = nullptr;             // HWND that owns the UAC prompt (recommended for GUIs)
    std::uint32_t connect_timeout_ms = 30'000; // helper start after the user accepted the prompt
    std::uint32_t io_timeout_ms = 120'000;     // one request to the helper
};
void set_elevation_options(ElevationOptions options);

// Keeps the current privileged access alive while the returned token exists, without prompting.
// Windows: the elevated helper session, if one is running, so that devices can be closed and
// reopened (e.g. read-only, then read-write) with no second UAC prompt. Elsewhere, or with no
// helper running: an empty token. Hold it only across such a reopen: the helper keeps running
// (elevated) as long as a token or a device exists.
std::shared_ptr<void> hold_privileges() noexcept;

#ifdef _WIN32
// Entry point of the elevated helper process (see src/win32_helper_main.cpp).
int helper_main(int argc, wchar_t **argv) noexcept;
#endif

// Buffer aligned to kIoAlignment. Throws std::bad_alloc on allocation failure.
class AlignedBuffer {
  public:
    explicit AlignedBuffer(std::size_t size)
        : size_(size),
          data_(static_cast<std::byte *>(::operator new(size ? size : 1, std::align_val_t{kIoAlignment}))) {}
    ~AlignedBuffer() { ::operator delete(data_, std::align_val_t{kIoAlignment}); }
    AlignedBuffer(AlignedBuffer &&o) noexcept
        : size_(std::exchange(o.size_, 0)), data_(std::exchange(o.data_, nullptr)) {}
    AlignedBuffer(const AlignedBuffer &) = delete;
    AlignedBuffer &operator=(const AlignedBuffer &) = delete;
    AlignedBuffer &operator=(AlignedBuffer &&) = delete;

    std::byte *data() noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::span<std::byte> span() noexcept { return {data_, size_}; }

  private:
    std::size_t size_;
    std::byte *data_;
};

} // namespace rawdisk
