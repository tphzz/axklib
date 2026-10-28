// Windows backend: \\.\PhysicalDriveN devices and image files.
// Enumeration needs no privileges. Opening a device needs Administrator; when the process is
// not elevated, open_platform() transparently routes through the elevated helper (UAC).
#include "authorization_session.hpp"
#include "detail.hpp"
#include "rawdisk/block_device.hpp"
#include "remote.hpp"
#include "win32_common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <winioctl.h>

namespace rawdisk {
namespace {

using detail::fail;
using detail::Handle;
using detail::last_error;
using detail::win_error;

constexpr std::string_view kDrivePrefix = R"(\\.\PhysicalDrive)";

bool is_physical_drive_path(std::string_view p) {
    if (!p.starts_with(kDrivePrefix))
        return false;
    const auto digits = p.substr(kDrivePrefix.size());
    return !digits.empty() && digits.size() <= 4 &&
           std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; });
}

std::wstring widen_ascii(std::string_view s) { return {s.begin(), s.end()}; } // only used on validated paths

Handle open_path(const std::wstring &path, DWORD access, DWORD share, DWORD flags = 0) {
    return Handle{::CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING, flags, nullptr)};
}

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE;

struct Geometry {
    std::uint32_t sector_size = 0;
    std::uint64_t size_bytes = 0;
};

Result<Geometry> query_geometry(HANDLE h) {
    alignas(DISK_GEOMETRY_EX) std::byte buf[256]{};
    DWORD n = 0;
    if (!::DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, buf, sizeof buf, &n, nullptr))
        return fail(last_error());
    if (n < sizeof(DISK_GEOMETRY) + sizeof(LARGE_INTEGER))
        return fail(Errc::internal_error);
    const auto *g = reinterpret_cast<const DISK_GEOMETRY_EX *>(buf);
    if (g->DiskSize.QuadPart < 0)
        return fail(Errc::internal_error);
    return Geometry{static_cast<std::uint32_t>(g->Geometry.BytesPerSector),
                    static_cast<std::uint64_t>(g->DiskSize.QuadPart)};
}

struct Descriptor {
    bool usb = false;
    bool removable = false;
    std::string model;
    std::string serial;
};

Result<Descriptor> query_descriptor(HANDLE h) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    alignas(STORAGE_DEVICE_DESCRIPTOR) std::byte buf[1024]{};
    DWORD n = 0;
    if (!::DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &n, nullptr))
        return fail(last_error());
    if (n < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return fail(Errc::internal_error);

    const auto *d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buf);
    auto str_at = [&](DWORD off) -> std::string {
        if (off == 0 || off >= n)
            return {}; // 0 = absent; also rejects 0xFFFFFFFF
        const char *s = reinterpret_cast<const char *>(buf) + off;
        return detail::trim(std::string_view(s, ::strnlen(s, n - off)));
    };
    return Descriptor{d->BusType == BusTypeUsb, d->RemovableMedia != FALSE,
                      detail::join_model(str_at(d->VendorIdOffset), str_at(d->ProductIdOffset)),
                      str_at(d->SerialNumberOffset)};
}

Result<DWORD> disk_number(HANDLE h) {
    STORAGE_DEVICE_NUMBER num{};
    DWORD n = 0;
    if (!::DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &num, sizeof num, &n, nullptr))
        return fail(last_error());
    if (n < sizeof num || num.DeviceType != FILE_DEVICE_DISK)
        return fail(Errc::device_identity_unavailable);
    return num.DeviceNumber;
}

// Windows refuses raw writes into a mounted volume. Lock and dismount every volume on this disk;
// the returned handles keep them locked until they are closed.
Result<std::vector<Handle>> lock_and_dismount_volumes(DWORD disk) {
    std::vector<Handle> locked;
    wchar_t name[MAX_PATH]{};
    const HANDLE find = ::FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE)
        return locked; // no volumes at all
    struct FindCloser {
        HANDLE f;
        ~FindCloser() { ::FindVolumeClose(f); }
    } closer{find};

    do {
        std::wstring vol = name;
        if (!vol.empty() && vol.back() == L'\\')
            vol.pop_back(); // CreateFile wants no trailing '\'

        Handle probe = open_path(vol, 0, kShareAll);
        if (!probe.valid())
            continue;
        alignas(VOLUME_DISK_EXTENTS) std::byte buf[sizeof(VOLUME_DISK_EXTENTS) + 31 * sizeof(DISK_EXTENT)]{};
        DWORD n = 0;
        if (!::DeviceIoControl(probe.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof buf, &n, nullptr))
            continue; // e.g. optical drives; they cannot live on our disk
        const auto *ext = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buf);
        bool ours = false;
        for (DWORD i = 0; i < ext->NumberOfDiskExtents && i < 32; ++i)
            ours |= ext->Extents[i].DiskNumber == disk;
        if (!ours)
            continue;

        Handle v = open_path(vol, GENERIC_READ | GENERIC_WRITE, kShareAll);
        if (!v.valid()) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_ACCESS_DENIED && !detail::is_elevated())
                return fail(win_error(e)); // caller elevates
            return fail(Errc::device_busy);
        }
        // Locking can fail transiently (indexer, antivirus); retry briefly like Windows' own tools.
        DWORD m = 0;
        bool is_locked = false;
        for (int attempt = 0; attempt < 20 && !is_locked; ++attempt) {
            is_locked = ::DeviceIoControl(v.h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &m, nullptr) != FALSE;
            if (!is_locked)
                ::Sleep(100);
        }
        if (!is_locked)
            return fail(Errc::device_busy); // a program has files open on the card
        ::DeviceIoControl(v.h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &m, nullptr);
        locked.push_back(std::move(v));
    } while (::FindNextVolumeW(find, name, MAX_PATH));

    const DWORD e = ::GetLastError();
    if (e != ERROR_NO_MORE_FILES)
        return fail(win_error(e));
    return locked;
}

class Win32Disk final : public BlockDevice {
  public:
    Win32Disk(Handle h, std::vector<Handle> locked, std::uint32_t ss, std::uint64_t count, bool writable,
              bool is_device, std::optional<DeviceInfo> expected = {},
              std::optional<detail::Win32UsbIdentity> identity = {})
        : locked_(std::move(locked)), disk_(std::move(h)), ss_(ss), count_(count), writable_(writable),
          is_device_(is_device), expected_(std::move(expected)), identity_(std::move(identity)) {}

    ~Win32Disk() override {
        if (is_device_ && written_) { // let Windows re-read a partition table we may have changed
            DWORD n = 0;
            ::DeviceIoControl(disk_.h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &n, nullptr);
        }
        // disk_ is destroyed before locked_ (reverse declaration order): volumes unlock last.
    }

    std::uint32_t sector_size() const noexcept override { return ss_; }
    std::uint64_t sector_count() const noexcept override { return count_; }
    bool writable() const noexcept override { return writable_; }

  protected:
    Result<void> verify_binding_raw() override {
        if (!is_device_ || !expected_ || !identity_)
            return fail(Errc::not_a_usb_disk);
        const auto desc = query_descriptor(disk_.h);
        if (!desc)
            return fail(desc.error());
        const auto geo = query_geometry(disk_.h);
        if (!geo)
            return fail(geo.error());
        const auto number = disk_number(disk_.h);
        if (!number)
            return fail(number.error());
        if (std::format(R"(\\.\PhysicalDrive{})", *number) != expected_->path)
            return fail(Errc::device_connection_changed);
        if (!desc->usb || desc->model != expected_->model || desc->serial != expected_->serial ||
            desc->removable != expected_->removable)
            return fail(Errc::device_descriptor_changed);
        if (geo->sector_size != expected_->sector_size || geo->size_bytes != expected_->size_bytes)
            return fail(Errc::device_geometry_changed);
        return detail::win32_verify_usb_identity(*identity_);
    }

    Result<void> read_raw(std::uint64_t off, std::span<std::byte> out) override {
        OVERLAPPED ov = at(off);
        DWORD n = 0;
        if (!::ReadFile(disk_.h, out.data(), static_cast<DWORD>(out.size()), &n, &ov))
            return fail(last_error());
        if (n != out.size())
            return fail(Errc::device_changed); // medium shrank or vanished
        return {};
    }

    Result<void> write_raw(std::uint64_t off, std::span<const std::byte> in) override {
        written_ = true; // set first: a failed write may still have changed sectors
        OVERLAPPED ov = at(off);
        DWORD n = 0;
        if (!::WriteFile(disk_.h, in.data(), static_cast<DWORD>(in.size()), &n, &ov)) {
            const DWORD e = ::GetLastError();
            return fail(e == ERROR_WRITE_PROTECT ? make_error_code(Errc::write_protected) : win_error(e));
        }
        if (n != in.size())
            return fail(Errc::device_changed);
        return {};
    }

    Result<void> flush_raw() override {
        if (!::FlushFileBuffers(disk_.h))
            return fail(last_error());
        return {};
    }

  private:
    static OVERLAPPED at(std::uint64_t off) noexcept { // positional I/O on a synchronous handle
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        return ov;
    }

    std::vector<Handle> locked_;
    Handle disk_;
    std::uint32_t ss_;
    std::uint64_t count_;
    bool writable_;
    bool is_device_;
    bool written_ = false;
    std::optional<DeviceInfo> expected_;
    std::optional<detail::Win32UsbIdentity> identity_;
};

// Opens the device in this process. Fails with ERROR_ACCESS_DENIED when not elevated.
Result<std::unique_ptr<BlockDevice>> open_direct(const DeviceInfo &info, Access access) {
    const bool rw = access != Access::ReadOnly;
    if (!is_physical_drive_path(info.path))
        return fail(Errc::not_a_usb_disk);
    const std::wstring path = widen_ascii(info.path);

    std::vector<Handle> locked;
    if (rw) {
        Handle probe = open_path(path, 0, kShareAll);
        if (!probe.valid())
            return fail(last_error());
        auto num = disk_number(probe.h);
        if (!num)
            return fail(num.error());
        if (access == Access::ReadWriteUnmounted) {
            auto inventory = detail::inventory_platform(info, false);
            if (!inventory || !inventory->complete || !inventory->non_dismounting_claim_supported)
                return fail(Errc::device_busy);
        } else {
            auto l = lock_and_dismount_volumes(*num);
            if (!l)
                return fail(l.error());
            locked = std::move(*l);
        }
    }

    Handle h = open_path(path, GENERIC_READ | (rw ? GENERIC_WRITE : 0), rw ? 0 : kShareAll,
                         FILE_FLAG_NO_BUFFERING | (rw ? FILE_FLAG_WRITE_THROUGH : 0));
    if (!h.valid()) {
        const auto error = ::GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
            return fail(Errc::device_busy);
        return fail(win_error(error));
    }

    // Identity on the opened handle: catches a device swapped since enumeration.
    auto desc = query_descriptor(h.h);
    if (!desc)
        return fail(desc.error());
    auto geo = query_geometry(h.h);
    if (!geo)
        return fail(geo.error());
    if (!desc->usb || desc->model != info.model || desc->serial != info.serial)
        return fail(Errc::device_descriptor_changed);
    if (geo->sector_size != info.sector_size || geo->size_bytes != info.size_bytes)
        return fail(Errc::device_geometry_changed);
    const auto identity = detail::win32_usb_identity(h.h);
    if (!identity)
        return fail(identity.error());
    if (identity->carrier_id != info.carrier_id || identity->connection_id != info.connection_id)
        return fail(Errc::device_connection_changed);
    if (!detail::valid_sector_size(geo->sector_size) || geo->size_bytes % geo->sector_size != 0)
        return fail(Errc::device_geometry_changed);

    if (rw) {
        DWORD n = 0;
        if (!::DeviceIoControl(h.h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &n, nullptr)) {
            const DWORD e = ::GetLastError();
            return fail(e == ERROR_WRITE_PROTECT ? make_error_code(Errc::write_protected) : win_error(e));
        }
    }

    return std::make_unique<Win32Disk>(std::move(h), std::move(locked), geo->sector_size,
                                       geo->size_bytes / geo->sector_size, rw, true, info, *identity);
}

bool is_access_denied(const std::error_code &ec) {
    return ec == std::error_code(ERROR_ACCESS_DENIED, std::system_category());
}

} // namespace

bool detail::is_elevated() noexcept {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    Handle token{tok};
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    return ::GetTokenInformation(token.h, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
}

Result<std::vector<DeviceInfo>> detail::list_platform() {
    std::vector<DeviceInfo> out;
    for (int i = 0; i < 128; ++i) { // numbering can have gaps: don't stop at the first miss
        const std::string path = std::format(R"(\\.\PhysicalDrive{})", i);
        Handle h = open_path(widen_ascii(path), 0, kShareAll); // access 0 = query only, no admin needed
        if (!h.valid())
            continue;

        auto desc = query_descriptor(h.h);
        if (!desc || !desc->usb)
            continue;
        auto geo = query_geometry(h.h);
        if (!geo || geo->size_bytes == 0 || !valid_sector_size(geo->sector_size) ||
            geo->size_bytes % geo->sector_size != 0)
            continue; // no card in the reader, or nonsense geometry

        DeviceInfo d;
        d.path = path;
        d.model = std::move(desc->model);
        d.serial = std::move(desc->serial);
        d.size_bytes = geo->size_bytes;
        d.sector_size = geo->sector_size;
        d.removable = desc->removable;
        auto identity = detail::win32_usb_identity(h.h);
        if (identity) {
            d.carrier_id = std::move(identity->carrier_id);
            d.connection_id = std::move(identity->connection_id);
        }
        // Unidentifiable disks are not selectable, but preserve the descriptor for stale-open diagnostics.
        out.push_back(std::move(d));
    }
    return out;
}

Result<DeviceInventory> detail::inventory_platform(const DeviceInfo &, bool) {
    DeviceInventory result;
    // PhysicalDrive sharing is not a volume mount-inhibition claim. Do not
    // inspect volume filesystem handles as a substitute for a retained mount guard.
    result.notices.push_back("Windows formatting is unavailable: complete non-dismounting mount inspection and "
                             "automatic-remount inhibition are not qualified.");
    return result;
}

Result<std::unique_ptr<BlockDevice>> detail::open_platform(const DeviceInfo &info, Access access,
                                                           AuthorizationSession &authorization) {
    auto direct = open_direct(info, access);
    if (direct || !is_access_denied(direct.error()))
        return direct;

    // Access denied: elevate only now, only if allowed, and only if that can help.
    const auto &options = AuthorizationSessionAccess::options(authorization);
    if (!options.allow || is_elevated())
        return direct;

    auto session = win32_elevated_session(authorization);
    if (!session)
        return fail(session.error());
    return (*session)->open(info, access); // the helper repeats every check on its side
}

Result<std::unique_ptr<BlockDevice>> detail::open_image_platform(const std::filesystem::path &path, Access access,
                                                                 std::uint32_t sector_size) {
    const bool rw = access != Access::ReadOnly;
    const std::wstring &w = path.native();

    // Never let an "image" path reach a device: that would bypass every device check.
    for (const wchar_t *prefix : {LR"(\\.\)", LR"(\\?\GLOBALROOT)", LR"(\\?\Volume{)"})
        if (::_wcsnicmp(w.c_str(), prefix, std::wcslen(prefix)) == 0)
            return fail(Errc::invalid_argument);
    const DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return fail(last_error());
    if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))
        return fail(Errc::invalid_argument);

    // Share mode is the lock: nobody else may write while we have it open.
    Handle h{::CreateFileW(w.c_str(), GENERIC_READ | (rw ? GENERIC_WRITE : 0), FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!h.valid()) {
        const DWORD e = ::GetLastError();
        return fail(e == ERROR_SHARING_VIOLATION ? make_error_code(Errc::device_busy) : win_error(e));
    }
    if (::GetFileType(h.h) != FILE_TYPE_DISK)
        return fail(Errc::invalid_argument);

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h.h, &size))
        return fail(last_error());
    if (size.QuadPart <= 0)
        return fail(Errc::invalid_argument);
    const auto bytes = static_cast<std::uint64_t>(size.QuadPart);
    if (bytes % sector_size != 0)
        return fail(Errc::bad_length);

    return std::make_unique<Win32Disk>(std::move(h), std::vector<Handle>{}, sector_size, bytes / sector_size, rw,
                                       false);
}

} // namespace rawdisk
