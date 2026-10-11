// Raw device + image backend for Linux (/dev/sdX) and macOS (/dev/rdiskN).
#include "detail.hpp"
#include "posix_authorization.hpp"
#include "rawdisk/block_device.hpp"

#include <cerrno>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include "linux_authorization.hpp"
#include <linux/fs.h>
#include <sys/sysmacros.h>
#elif defined(__APPLE__)
#include <sys/disk.h>
#include <sys/mount.h>
#else
#error "posix_disk.cpp supports Linux and macOS only"
#endif

namespace rawdisk {

#if defined(__APPLE__)
namespace detail {
std::optional<DeviceInfo> macos_describe(const std::string &bsd_name); // enumerate_macos.cpp
}
#endif

namespace {

std::error_code last_errno() { return detail::errno_code(errno); }

using Fd = detail::PosixFd;

class PosixDisk final : public BlockDevice {
  public:
    PosixDisk(Fd fd, std::uint32_t ss, std::uint64_t count, bool writable, bool is_device)
        : fd_(std::move(fd)), ss_(ss), count_(count), writable_(writable), is_device_(is_device) {}

    ~PosixDisk() override {
#if defined(__linux__)
        // Let the kernel re-read a partition table we may have rewritten (best effort).
        if (is_device_ && written_)
            ::ioctl(fd_.get(), BLKRRPART);
#endif
    }

    std::uint32_t sector_size() const noexcept override { return ss_; }
    std::uint64_t sector_count() const noexcept override { return count_; }
    bool writable() const noexcept override { return writable_; }

  protected:
    Result<void> read_raw(std::uint64_t off, std::span<std::byte> out) override {
        for (std::size_t done = 0; done < out.size();) {
            const ssize_t n = ::pread(fd_.get(), out.data() + done, out.size() - done, static_cast<off_t>(off + done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return detail::fail(last_errno());
            }
            if (n == 0)
                return detail::fail(Errc::device_changed); // medium shrank or vanished
            done += static_cast<std::size_t>(n);
        }
        return {};
    }

    Result<void> write_raw(std::uint64_t off, std::span<const std::byte> in) override {
        written_ = true; // set first: a failed write may still have changed sectors
        for (std::size_t done = 0; done < in.size();) {
            const ssize_t n = ::pwrite(fd_.get(), in.data() + done, in.size() - done, static_cast<off_t>(off + done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return detail::fail(last_errno());
            }
            if (n == 0)
                return detail::fail(Errc::device_changed);
            done += static_cast<std::size_t>(n);
        }
        return {};
    }

    Result<void> flush_raw() override {
        if (::fsync(fd_.get()) != 0)
            return detail::fail(last_errno());
#if defined(__APPLE__)
        if (is_device_)
            ::ioctl(fd_.get(), DKIOCSYNCHRONIZECACHE); // reader's own cache, best effort
#endif
        return {};
    }

  private:
    Fd fd_;
    std::uint32_t ss_;
    std::uint64_t count_;
    bool writable_;
    bool is_device_;
    bool written_ = false;
};

struct Geometry {
    std::uint32_t sector_size = 0;
    std::uint64_t sector_count = 0;
};

Result<Geometry> query_geometry(int fd) {
#if defined(__linux__)
    int ss = 0;
    std::uint64_t bytes = 0;
    if (::ioctl(fd, BLKSSZGET, &ss) != 0 || ::ioctl(fd, BLKGETSIZE64, &bytes) != 0)
        return detail::fail(last_errno());
    if (!detail::valid_sector_size(static_cast<std::uint64_t>(ss < 0 ? 0 : ss)))
        return detail::fail(Errc::device_changed);
    return Geometry{static_cast<std::uint32_t>(ss), bytes / static_cast<std::uint64_t>(ss)};
#else
    std::uint32_t ss = 0;
    std::uint64_t count = 0;
    if (::ioctl(fd, DKIOCGETBLOCKSIZE, &ss) != 0 || ::ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0)
        return detail::fail(last_errno());
    if (!detail::valid_sector_size(ss))
        return detail::fail(Errc::device_changed);
    return Geometry{ss, count};
#endif
}

// Identity check on the *opened* descriptor, so a device swapped between enumeration and
// open() is caught.
Result<void> verify_identity(int fd, const DeviceInfo &info) {
    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return detail::fail(last_errno());
#if defined(__linux__)
    if (!S_ISBLK(st.st_mode))
        return detail::fail(Errc::not_a_usb_disk);
    const auto entry = detail::sysfs_root() / "dev/block" / std::format("{}:{}", major(st.st_rdev), minor(st.st_rdev));
    const auto now = detail::linux_describe_block(entry);
#else
    if (!S_ISCHR(st.st_mode))
        return detail::fail(Errc::not_a_usb_disk);
    const char *name = ::devname(st.st_rdev, S_IFCHR); // "rdisk4"
    if (!name || std::string_view(name).substr(0, 5) != "rdisk")
        return detail::fail(Errc::not_a_usb_disk);
    const auto now = detail::macos_describe(std::string(name + 1)); // "disk4"
#endif
    if (!now || !detail::same_identity(*now, info))
        return detail::fail(Errc::device_changed);
    return {};
}

#if defined(__linux__)

Result<Fd> open_linux(const DeviceInfo &info, bool rw) {
    // O_EXCL on a block device is an exclusive open: it fails with EBUSY while the disk or
    // any of its partitions is mounted or claimed, so we can never write under a filesystem.
    const int flags = (rw ? O_RDWR | O_EXCL : O_RDONLY) | O_DIRECT | O_CLOEXEC | O_NOCTTY;
    Fd fd(::open(info.path.c_str(), flags));
    if (fd.valid())
        return fd;

    const int e = errno;
    if (rw && e == EBUSY)
        return detail::fail(Errc::device_busy);
    if (e == EROFS)
        return detail::fail(Errc::write_protected);
    if (rw && e == EACCES) { // write-protected media also shows up as EACCES
        Fd ro(::open(info.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY));
        int readonly = 0;
        if (ro.valid() && ::ioctl(ro.get(), BLKROGET, &readonly) == 0 && readonly)
            return detail::fail(Errc::write_protected);
    }
    if ((e == EACCES || e == EPERM) && detail::elevation_options().allow)
        return detail::linux_authorized_open(info.path, rw ? Access::ReadWrite : Access::ReadOnly);
    return detail::fail(detail::errno_code(e));
}

#else // __APPLE__

// Refuses writes while any volume of this disk is mounted. getfsstat with our own buffer is
// thread-safe (getmntinfo is not).
Result<void> check_not_mounted(const std::string &raw_path) {
    std::string base = raw_path; // "/dev/rdisk4" -> "/dev/disk4"; volumes are disk4 or disk4sN
    if (base.starts_with("/dev/rdisk"))
        base.replace(0, 10, "/dev/disk");

    std::vector<struct statfs> mounts;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int n = ::getfsstat(nullptr, 0, MNT_NOWAIT);
        if (n < 0)
            return detail::fail(last_errno());
        mounts.resize(static_cast<std::size_t>(n) + 8);
        const int got = ::getfsstat(mounts.data(), static_cast<int>(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
        if (got < 0)
            return detail::fail(last_errno());
        if (static_cast<std::size_t>(got) < mounts.size()) {
            mounts.resize(static_cast<std::size_t>(got));
            break;
        }
    }
    for (const auto &m : mounts) {
        const std::string_view from = m.f_mntfromname;
        if (from.starts_with(base) && (from.size() == base.size() || from[base.size()] == 's'))
            return detail::fail(Errc::device_busy);
    }
    return {};
}

Result<Fd> open_macos(const DeviceInfo &info, bool rw) {
    if (rw) {
        if (auto m = check_not_mounted(info.path); !m)
            return detail::fail(m.error());
    }
    const int flags = rw ? O_RDWR : O_RDONLY;
    Fd fd(::open(info.path.c_str(), flags | O_CLOEXEC | O_NOCTTY));
    if (fd.valid())
        return fd;
    const int e = errno;
    if ((e == EACCES || e == EPERM) && detail::elevation_options().allow)
        return detail::macos_authorized_open(info.path, rw ? Access::ReadWrite : Access::ReadOnly);
    if (e == EROFS)
        return detail::fail(Errc::write_protected);
    if (e == EBUSY)
        return detail::fail(Errc::device_busy);
    return detail::fail(detail::errno_code(e));
}

#endif

} // namespace

Result<std::unique_ptr<BlockDevice>> detail::open_platform(const DeviceInfo &info, Access access) {
    const bool rw = access == Access::ReadWrite;
    if (!info.path.starts_with("/dev/") || info.path.find("..") != std::string::npos)
        return fail(Errc::not_a_usb_disk);

#if defined(__linux__)
    auto fd = open_linux(info, rw);
#else
    auto fd = open_macos(info, rw);
#endif
    if (!fd)
        return fail(fd.error());

#if defined(__APPLE__)
    ::fcntl(fd->get(), F_NOCACHE, 1); // bypass the buffer cache
#endif

    if (auto r = verify_identity(fd->get(), info); !r)
        return fail(r.error());

    if (rw) {
#if defined(__linux__)
        int readonly = 0;
        if (::ioctl(fd->get(), BLKROGET, &readonly) != 0)
            return fail(last_errno());
        if (readonly)
            return fail(Errc::write_protected);
#else
        std::uint32_t is_writable = 0;
        if (::ioctl(fd->get(), DKIOCISWRITABLE, &is_writable) == 0 && !is_writable)
            return fail(Errc::write_protected);
        // Re-check after opening: closes the window in which Disk Arbitration could have mounted it.
        if (auto m = check_not_mounted(info.path); !m)
            return fail(m.error());
#endif
    }

    auto geo = query_geometry(fd->get());
    if (!geo)
        return fail(geo.error());
    if (geo->sector_size != info.sector_size || std::uint64_t{geo->sector_size} * geo->sector_count != info.size_bytes)
        return fail(Errc::device_changed);

    return std::make_unique<PosixDisk>(std::move(*fd), geo->sector_size, geo->sector_count, rw, true);
}

Result<std::unique_ptr<BlockDevice>> detail::open_image_platform(const std::filesystem::path &path, Access access,
                                                                 std::uint32_t sector_size) {
    const bool rw = access == Access::ReadWrite;
    Fd fd(::open(path.c_str(), (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOCTTY));
    if (!fd.valid())
        return fail(last_errno());

    struct stat st{};
    if (::fstat(fd.get(), &st) != 0)
        return fail(last_errno());
    if (!S_ISREG(st.st_mode))
        return fail(Errc::invalid_argument); // never raw devices through this path

    if (::flock(fd.get(), (rw ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        const int e = errno;
        return fail(e == EWOULDBLOCK ? make_error_code(Errc::device_busy) : errno_code(e));
    }

    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (size == 0)
        return fail(Errc::invalid_argument);
    if (size % sector_size != 0)
        return fail(Errc::bad_length);
    return std::make_unique<PosixDisk>(std::move(fd), sector_size, size / sector_size, rw, false);
}

// POSIX authorization returns a local descriptor; there is no application helper to keep alive.
std::shared_ptr<void> hold_privileges() noexcept { return nullptr; }

} // namespace rawdisk
