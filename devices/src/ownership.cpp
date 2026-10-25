#include "rawdisk/ownership.hpp"

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <system_error>

#include "detail.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <shlobj.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rawdisk {
namespace {

std::string lease_name(std::string_view identity) {
    // Collisions can only deny a lease, never grant concurrent access.
    std::uint64_t first = 14695981039346656037ULL;
    std::uint64_t second = 7809847782465536322ULL;
    for (const auto character : identity) {
        const auto byte = static_cast<unsigned char>(character);
        first = (first ^ byte) * 1099511628211ULL;
        second = (second ^ byte) * 1099511628211ULL;
    }
    return std::format("carrier-{:016x}{:016x}.lock", first, second);
}

struct Lease {
#if defined(_WIN32)
    HANDLE handle{INVALID_HANDLE_VALUE};
    HANDLE directory{INVALID_HANDLE_VALUE};
    ~Lease() {
        if (handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
        if (directory != INVALID_HANDLE_VALUE)
            ::CloseHandle(directory);
    }
#else
    int descriptor{-1};
    ~Lease() {
        if (descriptor >= 0)
            ::close(descriptor);
    }
#endif
};

} // namespace

Result<std::filesystem::path> carrier_lease_directory() noexcept {
    return detail::guard([]() -> Result<std::filesystem::path> {
#if defined(_WIN32)
        PWSTR directory{};
        const auto status = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &directory);
        if (FAILED(status))
            return detail::fail(std::make_error_code(std::errc::permission_denied));
        const auto result = std::filesystem::path{directory} / "axklib" / "device-leases";
        CoTaskMemFree(directory);
        return result;
#else
        return std::filesystem::path{"/tmp"} / std::format("axklib-device-leases-{}", ::geteuid());
#endif
    });
}

Result<std::shared_ptr<BlockDevice>> open_owned_device(const DeviceInfo &info, Access access,
                                                       std::shared_ptr<AuthorizationSession> authorization) noexcept {
    return detail::guard([&]() -> Result<std::shared_ptr<BlockDevice>> {
        auto directory = carrier_lease_directory();
        if (!directory)
            return detail::fail(directory.error());
        auto lease = claim_carrier(*directory, info.carrier_id);
        if (!lease)
            return detail::fail(lease.error());
        auto device = open_device(info, access, std::move(authorization));
        if (!device)
            return detail::fail(device.error());
        struct Owned {
            std::shared_ptr<void> lease;
            std::unique_ptr<BlockDevice> device;
        };
        auto owned = std::make_shared<Owned>(Owned{std::move(*lease), std::move(*device)});
        auto *pointer = owned->device.get();
        return std::shared_ptr<BlockDevice>{std::move(owned), pointer};
    });
}

Result<std::shared_ptr<void>> claim_carrier(const std::filesystem::path &directory,
                                            std::string_view carrier_id) noexcept {
    return detail::guard([&]() -> Result<std::shared_ptr<void>> {
        if (carrier_id.empty() || carrier_id.size() > 4096 || directory.empty())
            return detail::fail(std::make_error_code(std::errc::invalid_argument));
        std::error_code error;
        const auto created = std::filesystem::create_directories(directory, error);
        if (error)
            return detail::fail(error);
        const auto path = directory / lease_name(carrier_id);
        auto lease = std::make_shared<Lease>();
#if defined(_WIN32)
        static_cast<void>(created);
        lease->directory =
            ::CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION parent{};
        if (lease->directory == INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(lease->directory, &parent) ||
            (parent.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (parent.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return detail::fail(std::make_error_code(std::errc::permission_denied));
        lease->handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (lease->handle == INVALID_HANDLE_VALUE) {
            const auto failure = ::GetLastError();
            if (failure == ERROR_SHARING_VIOLATION || failure == ERROR_LOCK_VIOLATION)
                return detail::fail(Errc::device_busy);
            return detail::fail(std::error_code(static_cast<int>(failure), std::system_category()));
        }
        BY_HANDLE_FILE_INFORMATION attributes{};
        if (!::GetFileInformationByHandle(lease->handle, &attributes) || attributes.nNumberOfLinks != 1 ||
            (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return detail::fail(std::make_error_code(std::errc::permission_denied));
#else
        const auto parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (parent < 0)
            return detail::fail(detail::errno_code(errno));
        struct Parent {
            int fd;
            ~Parent() { ::close(fd); }
        } close_parent{parent};
        struct stat attributes{};
        if (::fstat(parent, &attributes) != 0)
            return detail::fail(detail::errno_code(errno));
        if (attributes.st_uid != ::geteuid())
            return detail::fail(std::make_error_code(std::errc::permission_denied));
        if (created && ::fchmod(parent, 0700) != 0)
            return detail::fail(detail::errno_code(errno));
        if (!created && (attributes.st_mode & (S_IWGRP | S_IWOTH)) != 0)
            return detail::fail(std::make_error_code(std::errc::permission_denied));
        lease->descriptor = ::openat(parent, path.filename().c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lease->descriptor < 0)
            return detail::fail(detail::errno_code(errno));
        if (::fstat(lease->descriptor, &attributes) != 0)
            return detail::fail(detail::errno_code(errno));
        if (!S_ISREG(attributes.st_mode) || attributes.st_uid != ::geteuid() || attributes.st_nlink != 1)
            return detail::fail(std::make_error_code(std::errc::permission_denied));
        if (::flock(lease->descriptor, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                return detail::fail(Errc::device_busy);
            return detail::fail(detail::errno_code(errno));
        }
#endif
        // Do not unlink lease files: replacing a locked inode would split ownership.
        return std::static_pointer_cast<void>(std::move(lease));
    });
}

} // namespace rawdisk
