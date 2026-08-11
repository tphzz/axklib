#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <hash-library/sha256.h>

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
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif
#include <unistd.h>
#endif

#include "axklib/application/filesystem.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"

namespace axk::app::filesystem_internal {

Error root_error(std::string message);
Error reference_error(std::string message, std::string_view relative_path);
Error output_exists_error(std::string message, std::string_view relative_path);
Error publication_error(std::string message, std::string_view relative_path);
Error entry_error(std::string code, std::string message, std::string_view relative_path);
bool valid_root_id(std::string_view value);
bool valid_portable_component(std::string_view value);
bool within(const std::filesystem::path &root, const std::filesystem::path &candidate);
Result<void> verify_no_link_components(const std::filesystem::path &root, const std::filesystem::path &candidate,
                                       std::string_view relative_path);
std::string entry_key(const DirectoryEntry &entry);
std::string encode_cursor(std::string_view value);
Result<std::string> decode_cursor(std::string_view value, std::string_view relative_path);
Result<std::filesystem::path> relative_path_from_utf8(std::string_view value);
Result<std::filesystem::path> entry_name_from_utf8(std::string_view value);

struct NativeDirectoryEntry {
    std::filesystem::path name;
    std::string utf8_name;
    DirectoryEntryKind kind{DirectoryEntryKind::file};
    std::optional<std::uint64_t> size;
};

using DirectoryVisitor = std::function<Result<bool>(const NativeDirectoryEntry &)>;

#if defined(_WIN32)
struct NativeHandleCloser {
    void operator()(void *handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using NativeHandle = std::unique_ptr<void, NativeHandleCloser>;

Result<NativeHandle> open_relative(HANDLE parent, const std::filesystem::path &name, ACCESS_MASK access,
                                   ULONG disposition, ULONG options, std::string_view relative_path);
Result<NativeHandle> reopen_directory(HANDLE directory, std::string_view relative_path);
Result<NativeHandle> open_parent(HANDLE root, const std::filesystem::path &relative, std::string_view relative_path);
Result<void> visit_directory(HANDLE directory, std::string_view relative_path, const DirectoryVisitor &visitor);
Result<std::vector<std::byte>> read_prefix(HANDLE directory, const NativeDirectoryEntry &entry,
                                           std::size_t maximum_bytes, std::string_view relative_path);
#else
struct DescriptorCloser {
    void operator()(int *descriptor) const noexcept {
        if (descriptor != nullptr) {
            ::close(*descriptor);
            delete descriptor;
        }
    }
};
using NativeHandle = std::unique_ptr<int, DescriptorCloser>;

NativeHandle descriptor_handle(int descriptor);
Result<NativeHandle> open_parent(int root, const std::filesystem::path &relative, std::string_view relative_path);
Result<void> visit_directory(int directory, std::string_view relative_path, const DirectoryVisitor &visitor);
Result<std::vector<std::byte>> read_prefix(int directory, const NativeDirectoryEntry &entry, std::size_t maximum_bytes,
                                           std::string_view relative_path);
int rename_no_replace(int source_parent, const char *source_name, int destination_parent, const char *destination_name);
int rename_exchange(int parent, const char *first, const char *second);
#endif

struct NativeIdentity {
#if defined(_WIN32)
    std::uint64_t volume_serial{};
    std::array<std::byte, sizeof(FILE_ID_128)> file_id{};
    std::uint64_t size{};
    std::int64_t last_write_time{};
    std::int64_t change_time{};
#else
    std::uint64_t device{};
    std::uint64_t inode{};
    std::uint64_t size{};
    std::int64_t modification_seconds{};
    std::int64_t modification_nanoseconds{};
    std::int64_t change_seconds{};
    std::int64_t change_nanoseconds{};
#endif
    friend bool operator==(const NativeIdentity &, const NativeIdentity &) = default;
};

bool same_file_revision(const NativeIdentity &left, const NativeIdentity &right) noexcept;
std::string revision_token(const NativeIdentity &identity);
#if defined(_WIN32)
Result<NativeIdentity> native_identity(HANDLE handle, std::string_view relative_path);
#else
NativeIdentity native_identity(const struct stat &status);
Result<NativeIdentity> native_identity(int descriptor, std::string_view relative_path);
#endif

class NativeFileReader final : public axk::RandomAccessReader {
  public:
    NativeFileReader(NativeHandle handle, std::uint64_t size, std::string source_name)
        : handle_(std::move(handle)), size_(size), source_name_(std::move(source_name)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] axk::Result<void> read_exact_at(std::uint64_t offset,
                                                  std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset) {
            axk::ErrorContext context;
            context.source_path = source_name_;
            context.raw_offset = offset;
            return std::unexpected{axk::make_error(axk::ErrorCode::io_short_read, axk::ErrorCategory::io,
                                                   "sandbox file read exceeds available data", std::move(context))};
        }
        std::scoped_lock lock{mutex_};
#if defined(_WIN32)
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
            return std::unexpected{axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::io,
                                                   "sandbox file offset exceeds the platform range")};
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN) == 0) {
            return std::unexpected{
                axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "sandbox file seek failed")};
        }
        auto remaining = destination;
        while (!remaining.empty()) {
            const auto chunk =
                static_cast<DWORD>(std::min<std::size_t>(remaining.size(), std::numeric_limits<DWORD>::max()));
            DWORD read{};
            if (ReadFile(handle_.get(), remaining.data(), chunk, &read, nullptr) == 0 || read == 0U) {
                return std::unexpected{axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                                                       "sandbox file read failed")};
            }
            remaining = remaining.subspan(read);
        }
#else
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return std::unexpected{axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::io,
                                                   "sandbox file offset exceeds the platform range")};
        }
        auto remaining = destination;
        auto position = static_cast<off_t>(offset);
        while (!remaining.empty()) {
            const auto read = ::pread(*handle_, remaining.data(), remaining.size(), position);
            if (read < 0 && errno == EINTR)
                continue;
            if (read <= 0) {
                return std::unexpected{axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                                                       "sandbox file read failed")};
            }
            const auto consumed = static_cast<std::size_t>(read);
            remaining = remaining.subspan(consumed);
            position += read;
        }
#endif
        return {};
    }

    [[nodiscard]] Result<void> verify_unchanged(const NativeIdentity &expected) const {
#if defined(_WIN32)
        auto current = native_identity(handle_.get(), source_name_);
#else
        auto current = native_identity(*handle_, source_name_);
#endif
        if (!current || !same_file_revision(*current, expected)) {
            return std::unexpected(
                entry_error("archive_source_changed", "directory entry changed while it was archived", source_name_));
        }
        return {};
    }

  private:
    NativeHandle handle_;
    std::uint64_t size_{};
    std::string source_name_;
    mutable std::mutex mutex_;
};

std::filesystem::path temporary_entry_name(const std::filesystem::path &destination);
#if defined(_WIN32)
NTSTATUS rename_open_entry(HANDLE entry, HANDLE parent, const std::filesystem::path &name, bool replace);
bool delete_open_tree(HANDLE directory, std::string_view relative_path);
std::optional<bool> directory_is_empty(HANDLE directory);
#else
struct PosixObjectIdentity {
    dev_t device{};
    ino_t inode{};
    friend bool operator==(const PosixObjectIdentity &, const PosixObjectIdentity &) = default;
};
PosixObjectIdentity object_identity(const struct stat &status);
std::optional<PosixObjectIdentity> object_identity_at(int parent, const std::filesystem::path &name);
bool retained_entry_matches(int parent, const std::filesystem::path &name, int retained);
bool delete_tree_at(int parent, const std::filesystem::path &name,
                    std::optional<PosixObjectIdentity> expected = std::nullopt);
std::optional<bool> directory_empty_at(int parent, const std::filesystem::path &name,
                                       std::optional<PosixObjectIdentity> expected = std::nullopt);
bool synchronize_directory_tree(int directory);
#endif

} // namespace axk::app::filesystem_internal

using namespace axk::app::filesystem_internal;

struct axk::app::Sandbox::NativeRoot {
#if defined(_WIN32)
    explicit NativeRoot(HANDLE value) : handle(value) {}
    ~NativeRoot() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    explicit NativeRoot(int value) : descriptor(value) {}
    ~NativeRoot() {
        if (descriptor >= 0)
            ::close(descriptor);
    }
    int descriptor{-1};
#endif
};

struct axk::app::SandboxTree::Implementation {
    NativeHandle root;
    std::vector<SandboxTreeEntry> entries;
    std::vector<NativeIdentity> identities;
};

struct axk::app::SandboxMutation::Implementation {
    FileRef reference;
    std::filesystem::path filename;
    NativeHandle parent;
    NativeHandle file;
    NativeIdentity identity;
    std::uint64_t size{};
    std::unique_lock<std::mutex> mutation_lock;
    mutable std::mutex io_mutex;
};
