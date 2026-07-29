#include "axklib/file_publication.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "axklib/utf8.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

extern "C" NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE file_handle, PIO_STATUS_BLOCK io_status_block,
                                                        PVOID file_information, ULONG length,
                                                        FILE_INFORMATION_CLASS file_information_class);
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace axk::detail {
namespace {

Result<void> identity_error() {
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "temporary output identity changed before publication")};
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

OwnedHandle open_retained_candidate(const std::filesystem::path &path,
                                    const BY_HANDLE_FILE_INFORMATION &expected_identity, DWORD desired_access) {
    OwnedHandle handle{CreateFileW(path.c_str(), desired_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                   nullptr)};
    if (!handle)
        return {};
    BY_HANDLE_FILE_INFORMATION identity{};
    if (GetFileInformationByHandle(handle.get(), &identity) == 0 ||
        (identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        !same_identity(expected_identity, identity)) {
        return {};
    }
    return handle;
}

void discard_unidentified_candidate(HANDLE &handle) noexcept {
    OwnedHandle deletion_handle{
        ReOpenFile(handle, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0)};
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
    if (!deletion_handle)
        return;
    FILE_DISPOSITION_INFO disposition{TRUE};
    static_cast<void>(
        SetFileInformationByHandle(deletion_handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
}
#else
Result<int> open_private_staging_directory(int output_parent_descriptor) {
    constexpr std::string_view staging_name{".axklib-publication"};
    if (::mkdirat(output_parent_descriptor, staging_name.data(), 0700) != 0 && errno != EEXIST) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create private publication staging")};
    }
    struct stat status{};
    if (::fstatat(output_parent_descriptor, staging_name.data(), &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0777U) != 0700U) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "publication staging is not an owner-only directory")};
    }
    const auto descriptor =
        ::openat(output_parent_descriptor, staging_name.data(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open private publication staging")};
    }
    return descriptor;
}

bool same_identity(const struct stat &left, const struct stat &right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
#endif

axk::PublicationWarning cleanup_warning() {
    return {.code = "publication_cleanup_incomplete",
            .message = "output was committed but temporary publication cleanup did not complete"};
}

axk::PublicationWarning durability_warning() {
    return {.code = "publication_durability_unconfirmed",
            .message = "output was committed but directory synchronization did not complete"};
}

} // namespace

struct TemporaryPublication::Impl {
    std::filesystem::path candidate_path;
    std::filesystem::path destination_path;
    std::shared_ptr<const PublicationHooks> hooks;
    std::uint64_t append_offset{};
    bool active{true};
#if defined(_WIN32)
    HANDLE handle{INVALID_HANDLE_VALUE};
    HANDLE output_parent_handle{INVALID_HANDLE_VALUE};
    BY_HANDLE_FILE_INFORMATION identity{};
#else
    int descriptor{-1};
    int candidate_parent_descriptor{-1};
    int output_parent_descriptor{-1};
    struct stat identity{};
#endif

    ~Impl() {
#if defined(_WIN32)
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        if (output_parent_handle != INVALID_HANDLE_VALUE)
            CloseHandle(output_parent_handle);
#else
        if (descriptor >= 0)
            ::close(descriptor);
        if (candidate_parent_descriptor >= 0)
            ::close(candidate_parent_descriptor);
        if (output_parent_descriptor >= 0)
            ::close(output_parent_descriptor);
#endif
    }
};

TemporaryPublication::TemporaryPublication(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

TemporaryPublication::TemporaryPublication(TemporaryPublication &&other) noexcept = default;

TemporaryPublication &TemporaryPublication::operator=(TemporaryPublication &&other) noexcept {
    if (this != &other) {
        if (impl_ != nullptr && impl_->active)
            static_cast<void>(discard());
        impl_ = std::move(other.impl_);
    }
    return *this;
}

TemporaryPublication::~TemporaryPublication() {
    if (impl_ != nullptr && impl_->active)
        static_cast<void>(discard());
}

Result<TemporaryPublication> TemporaryPublication::create(const std::filesystem::path &destination) {
    return create(destination, std::shared_ptr<const PublicationHooks>{});
}

Result<TemporaryPublication> TemporaryPublication::create(const std::filesystem::path &destination,
                                                          std::shared_ptr<const PublicationHooks> hooks) {
    for (std::size_t attempt = 0; attempt < 64U; ++attempt) {
        auto candidate = text::temporary_sibling(destination);
        if (!candidate)
            return std::unexpected{candidate.error()};
#if defined(_WIN32)
        const auto parent = destination.parent_path().empty() ? std::filesystem::path{"."} : destination.parent_path();
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
        const auto handle = CreateFileW(candidate->c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            CloseHandle(parent_handle);
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "could not exclusively reserve a temporary output")};
        }
        auto impl = std::make_unique<Impl>();
        impl->candidate_path = std::move(*candidate);
        impl->destination_path = destination;
        impl->hooks = hooks;
        impl->handle = handle;
        impl->output_parent_handle = parent_handle;
        if (GetFileInformationByHandle(handle, &impl->identity) == 0) {
            discard_unidentified_candidate(impl->handle);
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify a temporary output")};
        }
#else
        const auto parent = destination.parent_path().empty() ? std::filesystem::path{"."} : destination.parent_path();
        const auto output_parent_descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (output_parent_descriptor < 0)
            return std::unexpected{identity_error().error()};
        auto candidate_parent = open_private_staging_directory(output_parent_descriptor);
        if (!candidate_parent) {
            ::close(output_parent_descriptor);
            return std::unexpected{candidate_parent.error()};
        }
        *candidate = parent / ".axklib-publication" / candidate->filename();
        const auto descriptor = ::openat(*candidate_parent, candidate->filename().c_str(),
                                         O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            const auto error = errno;
            ::close(*candidate_parent);
            ::close(output_parent_descriptor);
            if (error == EEXIST)
                continue;
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "could not exclusively reserve a temporary output")};
        }
        auto impl = std::make_unique<Impl>();
        impl->candidate_path = std::move(*candidate);
        impl->destination_path = destination;
        impl->hooks = hooks;
        impl->descriptor = descriptor;
        impl->candidate_parent_descriptor = *candidate_parent;
        impl->output_parent_descriptor = output_parent_descriptor;
        if (::fstat(descriptor, &impl->identity) != 0) {
            static_cast<void>(
                ::unlinkat(impl->candidate_parent_descriptor, impl->candidate_path.filename().c_str(), 0));
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not identify a temporary output")};
        }
#endif
        return TemporaryPublication{std::move(impl)};
    }
    return std::unexpected{
        make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not reserve a unique temporary output")};
}

Result<TemporaryPublication> TemporaryPublication::create(const std::filesystem::path &destination,
                                                          const TemporaryFileProducer &producer) {
    auto publication = create(destination);
    if (!publication)
        return std::unexpected{publication.error()};
    const TemporaryFileSink sink = [&publication](std::span<const std::byte> bytes) {
        return publication->append(bytes);
    };
    if (auto produced = producer(sink); !produced)
        return std::unexpected{produced.error()};
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};
    return std::move(*publication);
}

const std::filesystem::path &TemporaryPublication::path() const noexcept {
    static const std::filesystem::path empty;
    return impl_ == nullptr ? empty : impl_->candidate_path;
}

Result<void> TemporaryPublication::append(std::span<const std::byte> bytes) {
    if (impl_ == nullptr || !impl_->active)
        return identity_error();
    const auto offset = impl_->append_offset;
    if (auto written = write_at(offset, bytes); !written)
        return written;
    impl_->append_offset += bytes.size();
    return {};
}

Result<void> TemporaryPublication::write_at(std::uint64_t offset, std::span<const std::byte> bytes) {
    if (impl_ == nullptr || !impl_->active)
        return identity_error();
    while (!bytes.empty()) {
#if defined(_WIN32)
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "temporary output offset is unsupported")};
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN) == 0) {
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not seek temporary output")};
        }
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (WriteFile(impl_->handle, bytes.data(), chunk, &written, nullptr) == 0 || written == 0U) {
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary output")};
        }
        const auto count = static_cast<std::size_t>(written);
#else
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "temporary output offset is unsupported")};
        }
        const auto written = ::pwrite(impl_->descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
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

Result<void> TemporaryPublication::resize(std::uint64_t size) {
    if (impl_ == nullptr || !impl_->active)
        return identity_error();
#if defined(_WIN32)
    if (size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
        return std::unexpected{
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::io, "temporary output size is unsupported")};
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(size);
    if (SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN) == 0 || SetEndOfFile(impl_->handle) == 0)
#else
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        ::ftruncate(impl_->descriptor, static_cast<off_t>(size)) != 0)
#endif
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not resize temporary output")};
    impl_->append_offset = size;
    return {};
}

Result<void> TemporaryPublication::flush() {
    if (impl_ == nullptr || !impl_->active)
        return identity_error();
#if defined(_WIN32)
    const auto flushed = FlushFileBuffers(impl_->handle) != 0;
#else
    const auto flushed = ::fsync(impl_->descriptor) == 0;
#endif
    if (!flushed) {
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not flush temporary output to disk")};
    }
    return {};
}

Result<axk::PublicationOutcome> TemporaryPublication::publish(PublicationMode mode) {
    if (impl_ == nullptr || !impl_->active)
        return std::unexpected{identity_error().error()};
    axk::PublicationOutcome outcome;
#if defined(_WIN32)
    const auto publication_handle =
        open_retained_candidate(impl_->candidate_path, impl_->identity, DELETE | FILE_READ_ATTRIBUTES);
    if (!publication_handle)
        return std::unexpected{identity_error().error()};

    if (impl_->hooks && impl_->hooks->before_publish)
        impl_->hooks->before_publish();
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
    const auto filename = impl_->destination_path.filename().native();
    std::vector<std::byte> rename_buffer(sizeof(FILE_RENAME_INFO) + filename.size() * sizeof(wchar_t));
    auto *rename_info = reinterpret_cast<FILE_RENAME_INFO *>(rename_buffer.data());
    rename_info->ReplaceIfExists = mode == PublicationMode::replace_existing ? TRUE : FALSE;
    rename_info->RootDirectory = impl_->output_parent_handle;
    rename_info->FileNameLength = static_cast<DWORD>(filename.size() * sizeof(wchar_t));
    std::memcpy(rename_info->FileName, filename.data(), rename_info->FileNameLength);
    IO_STATUS_BLOCK status{};
    constexpr auto rename_information_class = static_cast<FILE_INFORMATION_CLASS>(10);
    const auto renamed = NtSetInformationFile(publication_handle.get(), &status, rename_info,
                                              static_cast<ULONG>(rename_buffer.size()), rename_information_class);
    if (renamed < 0) {
        const std::error_code error{static_cast<int>(RtlNtStatusToDosError(renamed)), std::system_category()};
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not atomically publish output: " + error.message())};
    }
    impl_->active = false;
#else
    struct stat candidate_identity{};
    const auto candidate_name = impl_->candidate_path.filename();
    const auto output_name = impl_->destination_path.filename();
    const auto valid = ::fstatat(impl_->candidate_parent_descriptor, candidate_name.c_str(), &candidate_identity,
                                 AT_SYMLINK_NOFOLLOW) == 0 &&
                       S_ISREG(candidate_identity.st_mode) && same_identity(candidate_identity, impl_->identity);
    if (!valid)
        return std::unexpected{identity_error().error()};

    if (impl_->hooks && impl_->hooks->before_publish)
        impl_->hooks->before_publish();
    if (mode == PublicationMode::replace_existing) {
        if (::renameat(impl_->candidate_parent_descriptor, candidate_name.c_str(), impl_->output_parent_descriptor,
                       output_name.c_str()) != 0) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not atomically replace output")};
        }
        impl_->active = false;
    } else {
        if (::linkat(impl_->candidate_parent_descriptor, candidate_name.c_str(), impl_->output_parent_descriptor,
                     output_name.c_str(), 0) != 0) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not atomically publish output")};
        }
        impl_->active = false;
        if (::unlinkat(impl_->candidate_parent_descriptor, candidate_name.c_str(), 0) != 0)
            outcome.warnings.push_back(cleanup_warning());
    }
    if (::fsync(impl_->output_parent_descriptor) != 0 || ::fsync(impl_->candidate_parent_descriptor) != 0) {
        outcome.durability = axk::PublicationDurability::unconfirmed;
        outcome.warnings.push_back(durability_warning());
    }
#endif
    return outcome;
}

Result<void> TemporaryPublication::discard() {
    if (impl_ == nullptr || !impl_->active)
        return {};
#if defined(_WIN32)
    const auto deletion_handle =
        open_retained_candidate(impl_->candidate_path, impl_->identity, DELETE | FILE_READ_ATTRIBUTES);
    if (!deletion_handle)
        return identity_error();
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (SetFileInformationByHandle(deletion_handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)) ==
        0) {
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not discard temporary output")};
    }
    impl_->active = false;
#else
    struct stat candidate_identity{};
    const auto candidate_name = impl_->candidate_path.filename();
    if (::fstatat(impl_->candidate_parent_descriptor, candidate_name.c_str(), &candidate_identity,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(candidate_identity.st_mode) || !same_identity(candidate_identity, impl_->identity)) {
        return identity_error();
    }
    if (::unlinkat(impl_->candidate_parent_descriptor, candidate_name.c_str(), 0) != 0) {
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not discard temporary output")};
    }
    impl_->active = false;
    if (::fsync(impl_->candidate_parent_descriptor) != 0) {
        return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                          "temporary output was discarded but directory synchronization failed")};
    }
#endif
    return {};
}

} // namespace axk::detail
