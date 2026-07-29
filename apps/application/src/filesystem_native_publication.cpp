#include "filesystem_internal.hpp"

namespace axk::app::filesystem_internal {

std::filesystem::path temporary_entry_name(const std::filesystem::path &destination) {
    static std::atomic<std::uint64_t> sequence{1U};
#if defined(_WIN32)
    const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process = static_cast<std::uint64_t>(::getpid());
#endif
    return std::filesystem::path{std::format("{}.axklib-publication.p{}.{}.tmp", axk::text::path_to_utf8(destination),
                                             process, sequence.fetch_add(1U, std::memory_order_relaxed))};
}

#if defined(_WIN32)

NTSTATUS rename_open_entry(HANDLE entry, HANDLE parent, const std::filesystem::path &name, bool replace) {
    const auto native = name.native();
    const auto bytes = sizeof(FILE_RENAME_INFO) + native.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(bytes);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->RootDirectory = parent;
    rename->FileNameLength = static_cast<DWORD>(native.size() * sizeof(wchar_t));
    std::copy(native.begin(), native.end(), rename->FileName);
    IO_STATUS_BLOCK status{};
    constexpr ULONG replace_if_exists = 0x1U;
    constexpr ULONG posix_semantics = 0x2U;
    const auto rename_information_class = static_cast<FILE_INFORMATION_CLASS>(replace ? 65 : 10);
    if (replace) {
        const ULONG flags = replace_if_exists | posix_semantics;
        std::memcpy(rename, &flags, sizeof(flags));
    } else {
        rename->ReplaceIfExists = FALSE;
    }
    return NtSetInformationFile(entry, &status, rename, static_cast<ULONG>(bytes), rename_information_class);
}

bool delete_open_tree(HANDLE directory, std::string_view relative_path) {
    std::vector<std::byte> buffer(64U * 1024U);
    while (true) {
        if (GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo, buffer.data(),
                                         static_cast<DWORD>(buffer.size())) == 0) {
            return GetLastError() == ERROR_NO_MORE_FILES;
        }
        auto *entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer.data());
        while (entry != nullptr) {
            const std::wstring_view name{entry->FileName, entry->FileNameLength / sizeof(wchar_t)};
            if (name != L"." && name != L"..") {
                const auto path = std::filesystem::path{name};
                const auto directory_entry = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
                auto child = open_relative(
                    directory, path,
                    DELETE | FILE_READ_ATTRIBUTES | (directory_entry ? FILE_LIST_DIRECTORY : FILE_READ_DATA), FILE_OPEN,
                    directory_entry ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE, relative_path);
                if (!child || (directory_entry && !delete_open_tree(child->get(), relative_path)))
                    return false;
                FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
                if (SetFileInformationByHandle(child->get(), FileDispositionInfo, &disposition, sizeof(disposition)) ==
                    0) {
                    return false;
                }
            }
            if (entry->NextEntryOffset == 0U)
                break;
            entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(reinterpret_cast<std::byte *>(entry) +
                                                              entry->NextEntryOffset);
        }
    }
}

std::optional<bool> directory_is_empty(HANDLE directory) {
    std::vector<std::byte> buffer(64U * 1024U);
    while (true) {
        if (GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo, buffer.data(),
                                         static_cast<DWORD>(buffer.size())) == 0) {
            return GetLastError() == ERROR_NO_MORE_FILES ? std::optional<bool>{true} : std::nullopt;
        }
        auto *entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer.data());
        while (entry != nullptr) {
            const std::wstring_view name{entry->FileName, entry->FileNameLength / sizeof(wchar_t)};
            if (name != L"." && name != L"..")
                return false;
            if (entry->NextEntryOffset == 0U)
                break;
            entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(reinterpret_cast<std::byte *>(entry) +
                                                              entry->NextEntryOffset);
        }
    }
}

#else

PosixObjectIdentity object_identity(const struct stat &status) { return {status.st_dev, status.st_ino}; }

std::optional<PosixObjectIdentity> object_identity_at(int parent, const std::filesystem::path &name) {
    struct stat status{};
    if (::fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(status.st_mode))
        return std::nullopt;
    return object_identity(status);
}

bool retained_entry_matches(int parent, const std::filesystem::path &name, int retained) {
    struct stat retained_status{};
    struct stat named_status{};
    return ::fstat(retained, &retained_status) == 0 &&
           ::fstatat(parent, name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISLNK(named_status.st_mode) &&
           object_identity(retained_status) == object_identity(named_status) &&
           (retained_status.st_mode & S_IFMT) == (named_status.st_mode & S_IFMT);
}

bool delete_tree_at(int parent, const std::filesystem::path &name, std::optional<PosixObjectIdentity> expected) {
    const auto descriptor = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    auto retained = descriptor_handle(descriptor);
    struct stat retained_status{};
    if (::fstat(*retained, &retained_status) != 0 || !S_ISDIR(retained_status.st_mode) ||
        (expected && object_identity(retained_status) != *expected)) {
        return false;
    }
    const auto enumeration_descriptor = ::dup(*retained);
    if (enumeration_descriptor < 0)
        return false;
    auto *directory = ::fdopendir(enumeration_descriptor);
    if (directory == nullptr) {
        ::close(enumeration_descriptor);
        return false;
    }
    bool removed = true;
    while (true) {
        errno = 0;
        const auto *entry = ::readdir(directory);
        if (entry == nullptr)
            break;
        const std::string_view child_name{entry->d_name};
        if (child_name == "." || child_name == "..")
            continue;
        struct stat status{};
        if (::fstatat(descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(status.st_mode)) {
            removed = false;
            break;
        }
        if (S_ISDIR(status.st_mode)) {
            if (!delete_tree_at(*retained, entry->d_name, object_identity(status))) {
                removed = false;
                break;
            }
        } else if (!S_ISREG(status.st_mode)) {
            removed = false;
            break;
        } else {
            const auto child_descriptor =
                ::openat(*retained, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (child_descriptor < 0) {
                removed = false;
                break;
            }
            auto child = descriptor_handle(child_descriptor);
            struct stat opened_status{};
            if (::fstat(*child, &opened_status) != 0 || !S_ISREG(opened_status.st_mode) ||
                object_identity(opened_status) != object_identity(status) ||
                !retained_entry_matches(*retained, entry->d_name, *child) ||
                ::unlinkat(*retained, entry->d_name, 0) != 0) {
                removed = false;
                break;
            }
        }
    }
    const auto enumeration_error = errno;
    ::closedir(directory);
    return removed && enumeration_error == 0 && retained_entry_matches(parent, name, *retained) &&
           ::unlinkat(parent, name.c_str(), AT_REMOVEDIR) == 0;
}

std::optional<bool> directory_empty_at(int parent, const std::filesystem::path &name,
                                       std::optional<PosixObjectIdentity> expected) {
    const auto descriptor = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return std::nullopt;
    auto retained = descriptor_handle(descriptor);
    struct stat retained_status{};
    if (::fstat(*retained, &retained_status) != 0 || !S_ISDIR(retained_status.st_mode) ||
        (expected && object_identity(retained_status) != *expected)) {
        return std::nullopt;
    }
    const auto enumeration_descriptor = ::dup(*retained);
    if (enumeration_descriptor < 0)
        return std::nullopt;
    auto *directory = ::fdopendir(enumeration_descriptor);
    if (directory == nullptr) {
        ::close(enumeration_descriptor);
        return std::nullopt;
    }
    bool empty = true;
    while (true) {
        errno = 0;
        const auto *entry = ::readdir(directory);
        if (entry == nullptr)
            break;
        const std::string_view child_name{entry->d_name};
        if (child_name != "." && child_name != "..") {
            empty = false;
            break;
        }
    }
    const auto enumeration_error = errno;
    ::closedir(directory);
    if (enumeration_error != 0 || !retained_entry_matches(parent, name, *retained))
        return std::nullopt;
    return empty;
}

bool synchronize_directory_tree(int directory) {
    const auto enumeration_descriptor = ::dup(directory);
    if (enumeration_descriptor < 0)
        return false;
    auto *entries = ::fdopendir(enumeration_descriptor);
    if (entries == nullptr) {
        ::close(enumeration_descriptor);
        return false;
    }
    bool synchronized = true;
    while (synchronized) {
        errno = 0;
        const auto *entry = ::readdir(entries);
        if (entry == nullptr)
            break;
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..")
            continue;
        struct stat status{};
        if (::fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(status.st_mode)) {
            synchronized = false;
            break;
        }
        if (!S_ISDIR(status.st_mode))
            continue;
        const auto child_descriptor =
            ::openat(directory, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child_descriptor < 0) {
            synchronized = false;
            break;
        }
        auto child = descriptor_handle(child_descriptor);
        struct stat opened_status{};
        synchronized = ::fstat(*child, &opened_status) == 0 && S_ISDIR(opened_status.st_mode) &&
                       object_identity(opened_status) == object_identity(status) &&
                       synchronize_directory_tree(*child) && retained_entry_matches(directory, entry->d_name, *child);
    }
    const auto enumeration_error = errno;
    ::closedir(entries);
    return synchronized && enumeration_error == 0 && ::fsync(directory) == 0;
}

#endif

} // namespace axk::app::filesystem_internal
