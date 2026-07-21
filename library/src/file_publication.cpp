#include "axklib/file_publication.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#include "axklib/utf8.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace axk::detail {
namespace {

struct RetainedFile {
    std::filesystem::path output_filename;
#if defined(_WIN32)
    HANDLE handle{INVALID_HANDLE_VALUE};
    HANDLE parent_handle{INVALID_HANDLE_VALUE};
    BY_HANDLE_FILE_INFORMATION identity{};
    ~RetainedFile() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        if (parent_handle != INVALID_HANDLE_VALUE)
            CloseHandle(parent_handle);
    }
#else
    int descriptor{-1};
    int parent_descriptor{-1};
    struct stat identity{};
    ~RetainedFile() {
        if (descriptor >= 0)
            ::close(descriptor);
        if (parent_descriptor >= 0)
            ::close(parent_descriptor);
    }
#endif
};

std::mutex retained_mutex;
std::map<std::filesystem::path, std::shared_ptr<RetainedFile>> retained_files;

std::filesystem::path retained_key(const std::filesystem::path &path) { return path.lexically_normal(); }

void retain(const std::filesystem::path &path, std::shared_ptr<RetainedFile> file) {
    const std::scoped_lock lock{retained_mutex};
    retained_files.insert_or_assign(retained_key(path), std::move(file));
}

std::shared_ptr<RetainedFile> release_retained(const std::filesystem::path &path) {
    const std::scoped_lock lock{retained_mutex};
    const auto found = retained_files.find(retained_key(path));
    if (found == retained_files.end())
        return {};
    auto result = std::move(found->second);
    retained_files.erase(found);
    return result;
}

std::shared_ptr<RetainedFile> find_retained(const std::filesystem::path &path) {
    const std::scoped_lock lock{retained_mutex};
    const auto found = retained_files.find(retained_key(path));
    return found == retained_files.end() ? nullptr : found->second;
}

#if defined(_WIN32)
bool same_identity(const BY_HANDLE_FILE_INFORMATION &left, const BY_HANDLE_FILE_INFORMATION &right) {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
}

class OwnedHandle {
  public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE value) : value_{value} {}
    OwnedHandle(const OwnedHandle &) = delete;
    OwnedHandle &operator=(const OwnedHandle &) = delete;
    OwnedHandle(OwnedHandle &&other) noexcept : value_{std::exchange(other.value_, INVALID_HANDLE_VALUE)} {}
    OwnedHandle &operator=(OwnedHandle &&other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE)
                CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~OwnedHandle() {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

OwnedHandle open_retained_candidate(const std::filesystem::path &path, const RetainedFile &retained,
                                    DWORD desired_access) {
    OwnedHandle handle{CreateFileW(path.c_str(), desired_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                   nullptr)};
    if (!handle)
        return {};
    BY_HANDLE_FILE_INFORMATION identity{};
    if (GetFileInformationByHandle(handle.get(), &identity) == 0 ||
        (identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        !same_identity(retained.identity, identity)) {
        return {};
    }
    return handle;
}
#endif

Result<void> identity_error() {
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "temporary output identity changed before publication")};
}

} // namespace

Result<std::filesystem::path> reserve_temporary_file(const std::filesystem::path &output) {
    for (std::size_t attempt = 0; attempt < 64U; ++attempt) {
        auto temporary = text::temporary_sibling(output);
        if (!temporary) {
            return std::unexpected{temporary.error()};
        }
#if defined(_WIN32)
        const auto parent = output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
        const auto parent_handle =
            CreateFileW(parent.c_str(), FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION parent_identity{};
        if (parent_handle == INVALID_HANDLE_VALUE || GetFileInformationByHandle(parent_handle, &parent_identity) == 0 ||
            (parent_identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
                FILE_ATTRIBUTE_DIRECTORY) {
            if (parent_handle != INVALID_HANDLE_VALUE)
                CloseHandle(parent_handle);
            return std::unexpected{identity_error().error()};
        }
        const auto handle = CreateFileW(temporary->c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            auto retained = std::make_shared<RetainedFile>();
            retained->output_filename = output.filename();
            retained->handle = handle;
            retained->parent_handle = parent_handle;
            if (GetFileInformationByHandle(handle, &retained->identity) == 0) {
                return std::unexpected{
                    make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify a temporary output")};
            }
            retain(*temporary, std::move(retained));
            return *temporary;
        }
        const auto error = GetLastError();
        CloseHandle(parent_handle);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "could not exclusively reserve a temporary output")};
        }
#else
        const auto parent = output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
        const auto parent_descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (parent_descriptor < 0)
            return std::unexpected{identity_error().error()};
        const auto descriptor = ::openat(parent_descriptor, temporary->filename().c_str(),
                                         O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor >= 0) {
            auto retained = std::make_shared<RetainedFile>();
            retained->output_filename = output.filename();
            retained->descriptor = descriptor;
            retained->parent_descriptor = parent_descriptor;
            if (::fstat(descriptor, &retained->identity) != 0) {
                return std::unexpected{
                    make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify a temporary output")};
            }
            retain(*temporary, std::move(retained));
            return *temporary;
        }
        ::close(parent_descriptor);
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
        const auto parent = output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
        const auto parent_handle =
            CreateFileW(parent.c_str(), FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION parent_identity{};
        if (parent_handle == INVALID_HANDLE_VALUE || GetFileInformationByHandle(parent_handle, &parent_identity) == 0 ||
            (parent_identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
                FILE_ATTRIBUTE_DIRECTORY) {
            if (parent_handle != INVALID_HANDLE_VALUE)
                CloseHandle(parent_handle);
            return std::unexpected{identity_error().error()};
        }
        const auto handle = CreateFileW(temporary->c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            CloseHandle(parent_handle);
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
        auto retained = std::make_shared<RetainedFile>();
        retained->output_filename = output.filename();
        retained->handle = handle;
        retained->parent_handle = parent_handle;
        if (GetFileInformationByHandle(handle, &retained->identity) == 0) {
            produced = std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify a temporary output")};
        }
#else
        const auto parent = output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
        const auto parent_descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (parent_descriptor < 0)
            return std::unexpected{identity_error().error()};
        const auto descriptor = ::openat(parent_descriptor, temporary->filename().c_str(),
                                         O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            ::close(parent_descriptor);
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
        auto retained = std::make_shared<RetainedFile>();
        retained->output_filename = output.filename();
        retained->descriptor = descriptor;
        retained->parent_descriptor = parent_descriptor;
        if (::fstat(descriptor, &retained->identity) != 0) {
            produced = std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify a temporary output")};
        }
#endif
        if (!produced || !flushed) {
            std::error_code ignored;
            std::filesystem::remove(*temporary, ignored);
            if (!produced)
                return std::unexpected{produced.error()};
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not flush temporary output to disk")};
        }
        retain(*temporary, std::move(retained));
        return *temporary;
    }
    return std::unexpected{
        make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create a unique temporary output")};
}

Result<void> resize_temporary_file(const std::filesystem::path &path, std::uint64_t size) {
    const auto retained = find_retained(path);
    if (!retained)
        return identity_error();
#if defined(_WIN32)
    if (size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
        return std::unexpected{
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::io, "temporary output size is unsupported")};
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(size);
    if (SetFilePointerEx(retained->handle, position, nullptr, FILE_BEGIN) == 0 || SetEndOfFile(retained->handle) == 0)
#else
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        ::ftruncate(retained->descriptor, static_cast<off_t>(size)) != 0)
#endif
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not resize temporary output")};
    return {};
}

Result<void> write_temporary_file_at(const std::filesystem::path &path, std::uint64_t offset,
                                     std::span<const std::byte> bytes) {
    const auto retained = find_retained(path);
    if (!retained)
        return identity_error();
    while (!bytes.empty()) {
#if defined(_WIN32)
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "temporary output offset is unsupported")};
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(retained->handle, position, nullptr, FILE_BEGIN) == 0) {
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not seek temporary output")};
        }
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (WriteFile(retained->handle, bytes.data(), chunk, &written, nullptr) == 0 || written == 0U) {
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary output")};
        }
        const auto count = static_cast<std::size_t>(written);
#else
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "temporary output offset is unsupported")};
        }
        const auto written = ::pwrite(retained->descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary output")};
        }
        const auto count = static_cast<std::size_t>(written);
#endif
        bytes = bytes.subspan(count);
        offset += count;
    }
    return {};
}

Result<void> flush_file_to_disk(const std::filesystem::path &path) {
    if (const auto retained = find_retained(path)) {
#if defined(_WIN32)
        if (FlushFileBuffers(retained->handle) == 0)
#else
        if (::fsync(retained->descriptor) != 0)
#endif
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not flush temporary output to disk")};
        return {};
    }
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

void discard_temporary_file(const std::filesystem::path &path) noexcept {
    const auto retained = release_retained(path);
#if defined(_WIN32)
    if (retained) {
        const auto deletion_handle = open_retained_candidate(path, *retained, DELETE | FILE_READ_ATTRIBUTES);
        if (!deletion_handle)
            return;
        if (retained->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(retained->handle);
            retained->handle = INVALID_HANDLE_VALUE;
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        static_cast<void>(
            SetFileInformationByHandle(deletion_handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        return;
    }
#else
    if (retained && retained->parent_descriptor >= 0 &&
        ::unlinkat(retained->parent_descriptor, path.filename().c_str(), 0) == 0) {
        return;
    }
#endif
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

Result<void> publish_temporary_file(const std::filesystem::path &temporary, const std::filesystem::path &output,
                                    bool overwrite) {
    const auto retained = find_retained(temporary);
    if (!retained || retained->output_filename != output.filename())
        return identity_error();
#if defined(_WIN32)
    const auto publication_handle = open_retained_candidate(temporary, *retained, DELETE | FILE_READ_ATTRIBUTES);
    if (!publication_handle) {
        static_cast<void>(release_retained(temporary));
        return identity_error();
    }
    if (retained->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(retained->handle);
        retained->handle = INVALID_HANDLE_VALUE;
    }
    const auto filename = output.filename().native();
    std::vector<std::byte> rename_buffer(sizeof(FILE_RENAME_INFO) + filename.size() * sizeof(wchar_t));
    auto *rename_info = reinterpret_cast<FILE_RENAME_INFO *>(rename_buffer.data());
    rename_info->ReplaceIfExists = overwrite ? TRUE : FALSE;
    // A simple name with no root handle renames within the open candidate's directory.
    rename_info->RootDirectory = nullptr;
    rename_info->FileNameLength = static_cast<DWORD>(filename.size() * sizeof(wchar_t));
    std::memcpy(rename_info->FileName, filename.data(), rename_info->FileNameLength);
    const auto renamed = SetFileInformationByHandle(publication_handle.get(), FileRenameInfo, rename_info,
                                                    static_cast<DWORD>(rename_buffer.size()));
    if (renamed == 0) {
        const std::error_code error{static_cast<int>(GetLastError()), std::system_category()};
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not atomically publish output: " + error.message())};
    }
#else
    const auto descriptor = retained->parent_descriptor;
    if (descriptor < 0)
        return identity_error();
    struct stat candidate_identity{};
    const auto temporary_name = temporary.filename();
    const auto output_name = output.filename();
    const auto valid = ::fstatat(descriptor, temporary_name.c_str(), &candidate_identity, AT_SYMLINK_NOFOLLOW) == 0 &&
                       S_ISREG(candidate_identity.st_mode) && candidate_identity.st_dev == retained->identity.st_dev &&
                       candidate_identity.st_ino == retained->identity.st_ino;
    if (!valid) {
        static_cast<void>(release_retained(temporary));
        return identity_error();
    }
    if (overwrite) {
        if (::renameat(descriptor, temporary_name.c_str(), descriptor, output_name.c_str()) != 0) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not atomically replace output")};
        }
    } else {
        if (::linkat(descriptor, temporary_name.c_str(), descriptor, output_name.c_str(), 0) != 0) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not atomically publish output")};
        }
        if (::unlinkat(descriptor, temporary_name.c_str(), 0) != 0) {
            return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                              "output was published but temporary cleanup failed")};
        }
    }
    if (::fsync(descriptor) != 0) {
        return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                          "output was published but its directory could not be synchronized")};
    }
#endif
    static_cast<void>(release_retained(temporary));
    return {};
}

} // namespace axk::detail
