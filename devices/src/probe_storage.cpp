#include "detail.hpp"
#include "probe_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rawdisk::probe::detail {

Result<std::vector<std::byte>> read_file(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return std::unexpected(ec ? ec : make_error_code(Errc::invalid_argument));
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return std::unexpected(ec);
    if (size > kMaxBytes + 1024 * 1024)
        return std::unexpected(make_error_code(Errc::bad_length));
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return std::unexpected(std::make_error_code(std::errc::io_error));
    if (in.peek() != std::char_traits<char>::eof())
        return std::unexpected(make_error_code(Errc::device_changed));
    return bytes;
}

Result<void> sync_directory(const std::filesystem::path &path) {
#ifdef _WIN32
    // CREATE_NEW + WRITE_THROUGH + FlushFileBuffers in write_new persists file data and metadata.
    // Windows does not provide POSIX directory fsync through a normal directory handle.
    (void)path;
    return {};
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(rawdisk::detail::errno_code(errno));
    const int rc = ::fsync(fd);
    const int saved = errno;
    const int closed = ::close(fd);
    if (rc < 0)
        return std::unexpected(rawdisk::detail::errno_code(saved));
    if (closed < 0)
        return std::unexpected(rawdisk::detail::errno_code(errno));
    return {};
#endif
}

Result<void> publication_barrier(const std::filesystem::path &path) {
#ifdef __APPLE__
    // Directory fsyncs must precede the FINAL hardware-cache barrier, not follow it.
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return std::unexpected(rawdisk::detail::errno_code(errno));
    const int rc = ::fcntl(fd, F_FULLFSYNC);
    const int saved = errno;
    const int closed = ::close(fd);
    if (rc < 0)
        return std::unexpected(rawdisk::detail::errno_code(saved));
    if (closed < 0)
        return std::unexpected(rawdisk::detail::errno_code(errno));
#else
    (void)path;
#endif
    return {};
}

Result<void> write_new(const std::filesystem::path &path, std::span<const std::byte> bytes) {
#ifdef _WIN32
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    std::error_code ec;
    for (std::size_t pos = 0; pos < bytes.size();) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - pos, 1024 * 1024));
        DWORD done = 0;
        if (!WriteFile(h, bytes.data() + pos, count, &done, nullptr)) {
            ec = {static_cast<int>(GetLastError()), std::system_category()};
            break;
        }
        if (done == 0) {
            ec = std::make_error_code(std::errc::io_error);
            break;
        }
        pos += done;
    }
    if (!ec && !FlushFileBuffers(h))
        ec = {static_cast<int>(GetLastError()), std::system_category()};
    if (!CloseHandle(h) && !ec)
        ec = {static_cast<int>(GetLastError()), std::system_category()};
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return std::unexpected(rawdisk::detail::errno_code(errno));
    std::error_code ec;
    for (std::size_t pos = 0; pos < bytes.size();) {
        const auto done = ::write(fd, bytes.data() + pos, bytes.size() - pos);
        if (done < 0 && errno == EINTR)
            continue;
        if (done <= 0) {
            ec = done < 0 ? rawdisk::detail::errno_code(errno) : std::make_error_code(std::errc::io_error);
            break;
        }
        pos += static_cast<std::size_t>(done);
    }
    if (!ec && ::fsync(fd) < 0)
        ec = rawdisk::detail::errno_code(errno);
#ifdef __APPLE__
    if (!ec && ::fcntl(fd, F_FULLFSYNC) < 0)
        ec = rawdisk::detail::errno_code(errno);
#endif
    if (::close(fd) < 0 && !ec)
        ec = rawdisk::detail::errno_code(errno);
#endif
    if (ec)
        return std::unexpected(ec);
    if (auto r = sync_directory(path.has_parent_path() ? path.parent_path() : std::filesystem::path(".")); !r)
        return r;
    return publication_barrier(path);
}

} // namespace rawdisk::probe::detail
