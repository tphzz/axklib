#include "axklib/file_publication.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

#include "axklib/utf8.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace axk::detail {

Result<std::filesystem::path> reserve_temporary_file(const std::filesystem::path &output) {
    for (std::size_t attempt = 0; attempt < 64U; ++attempt) {
        auto temporary = text::temporary_sibling(output);
        if (!temporary) {
            return std::unexpected{temporary.error()};
        }
#if defined(_WIN32)
        const auto handle = CreateFileW(temporary->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return *temporary;
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "could not exclusively reserve a temporary output")};
        }
#else
        const auto descriptor = ::open(temporary->c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor >= 0) {
            ::close(descriptor);
            return *temporary;
        }
        if (errno != EEXIST) {
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "could not exclusively reserve a temporary output")};
        }
#endif
    }
    return std::unexpected{
        make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not reserve a unique temporary output")};
}

Result<std::filesystem::path> write_temporary_file(const std::filesystem::path &output,
                                                   const TemporaryFileProducer &producer) {
    for (std::size_t attempt = 0; attempt < 64U; ++attempt) {
        auto temporary = text::temporary_sibling(output);
        if (!temporary)
            return std::unexpected{temporary.error()};
#if defined(_WIN32)
        const auto handle = CreateFileW(temporary->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "could not exclusively create a temporary output")};
        }
        const TemporaryFileSink sink = [handle](std::span<const std::byte> bytes) -> Result<void> {
            while (!bytes.empty()) {
                const auto chunk =
                    static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max()));
                DWORD written{};
                if (WriteFile(handle, bytes.data(), chunk, &written, nullptr) == 0 || written == 0U) {
                    return std::unexpected{
                        make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary output")};
                }
                bytes = bytes.subspan(written);
            }
            return {};
        };
        auto produced = producer(sink);
        const auto flushed = produced && FlushFileBuffers(handle) != 0;
        CloseHandle(handle);
#else
        const auto descriptor = ::open(temporary->c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST)
                continue;
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "could not exclusively create a temporary output")};
        }
        const TemporaryFileSink sink = [descriptor](std::span<const std::byte> bytes) -> Result<void> {
            while (!bytes.empty()) {
                const auto written = ::write(descriptor, bytes.data(), bytes.size());
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0) {
                    return std::unexpected{
                        make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary output")};
                }
                bytes = bytes.subspan(static_cast<std::size_t>(written));
            }
            return {};
        };
        auto produced = producer(sink);
        const auto flushed = produced && ::fsync(descriptor) == 0;
        ::close(descriptor);
#endif
        if (!produced || !flushed) {
            std::error_code ignored;
            std::filesystem::remove(*temporary, ignored);
            if (!produced)
                return std::unexpected{produced.error()};
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not flush temporary output to disk")};
        }
        return *temporary;
    }
    return std::unexpected{
        make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create a unique temporary output")};
}

Result<void> flush_file_to_disk(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open temporary output for flush")};
    }
    const auto flushed = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open temporary output for flush")};
    }
    const auto flushed = ::fsync(descriptor) == 0;
    ::close(descriptor);
#endif
    if (!flushed) {
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not flush temporary output to disk")};
    }
    return {};
}

Result<void> publish_temporary_file(const std::filesystem::path &temporary, const std::filesystem::path &output,
                                    bool overwrite) {
#if defined(_WIN32)
    const auto flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0U);
    if (MoveFileExW(temporary.c_str(), output.c_str(), flags) == 0) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not atomically publish output")};
    }
#else
    if (overwrite) {
        if (::rename(temporary.c_str(), output.c_str()) != 0) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not atomically replace output")};
        }
    } else {
        if (::link(temporary.c_str(), output.c_str()) != 0) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not atomically publish output")};
        }
        if (::unlink(temporary.c_str()) != 0) {
            return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                              "output was published but temporary cleanup failed")};
        }
    }
    const auto parent = output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
    const auto descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0 || ::fsync(descriptor) != 0) {
        if (descriptor >= 0)
            ::close(descriptor);
        return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                          "output was published but its directory could not be synchronized")};
    }
    ::close(descriptor);
#endif
    return {};
}

} // namespace axk::detail
