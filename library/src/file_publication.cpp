#include "axklib/file_publication.hpp"

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
