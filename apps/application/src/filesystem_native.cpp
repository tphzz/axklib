#include "filesystem_internal.hpp"

namespace axk::app::filesystem_internal {

#if defined(_WIN32)

axk::app::Result<NativeHandle> open_relative(HANDLE parent, const std::filesystem::path &name, ACCESS_MASK access,
                                             ULONG disposition, ULONG options, std::string_view relative_path) {
    auto text = name.native();
    if (text.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
        return std::unexpected(reference_error("sandbox path component is too long", relative_path));
    UNICODE_STRING unicode{.Length = static_cast<USHORT>(text.size() * sizeof(wchar_t)),
                           .MaximumLength = static_cast<USHORT>(text.size() * sizeof(wchar_t)),
                           .Buffer = text.data()};
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &unicode, OBJ_CASE_INSENSITIVE, parent, nullptr);
    IO_STATUS_BLOCK status{};
    HANDLE opened{};
    const auto result = NtCreateFile(&opened, access | SYNCHRONIZE, &attributes, &status, nullptr, 0U,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
                                     options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0U);
    if (result < 0)
        return std::unexpected(
            entry_error("entry_mutation_failed", "sandbox entry could not be opened safely", relative_path));
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(opened, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        CloseHandle(opened);
        return std::unexpected(reference_error("sandbox path contains a link component", relative_path));
    }
    return NativeHandle{opened};
}

axk::app::Result<NativeHandle> reopen_directory(HANDLE directory, std::string_view relative_path) {
    const auto required = GetFinalPathNameByHandleW(directory, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U)
        return std::unexpected(reference_error("sandbox directory path could not be resolved", relative_path));
    std::wstring path(required, L'\0');
    const auto length = GetFinalPathNameByHandleW(directory, path.data(), static_cast<DWORD>(path.size()),
                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0U || length >= path.size())
        return std::unexpected(reference_error("sandbox directory path changed while it was reopened", relative_path));
    path.resize(length);

    const auto reopened = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (reopened == INVALID_HANDLE_VALUE)
        return std::unexpected(reference_error("sandbox directory could not be reopened", relative_path));
    NativeHandle owned{reopened};

    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_ID_INFO retained_identity{};
    FILE_ID_INFO reopened_identity{};
    if (GetFileInformationByHandleEx(reopened, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        GetFileInformationByHandleEx(directory, FileIdInfo, &retained_identity, sizeof(retained_identity)) == 0 ||
        GetFileInformationByHandleEx(reopened, FileIdInfo, &reopened_identity, sizeof(reopened_identity)) == 0 ||
        retained_identity.VolumeSerialNumber != reopened_identity.VolumeSerialNumber ||
        std::memcmp(retained_identity.FileId.Identifier, reopened_identity.FileId.Identifier,
                    sizeof(retained_identity.FileId.Identifier)) != 0) {
        return std::unexpected(reference_error("sandbox directory changed while it was reopened", relative_path));
    }
    return owned;
}

axk::app::Result<NativeHandle> open_parent(HANDLE root, const std::filesystem::path &relative,
                                           std::string_view relative_path) {
    auto reopened = reopen_directory(root, relative_path);
    if (!reopened)
        return std::unexpected(reopened.error());
    NativeHandle owned = std::move(*reopened);
    HANDLE current = owned.get();
    for (const auto &component : relative) {
        auto next = open_relative(current, component, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN,
                                  FILE_DIRECTORY_FILE, relative_path);
        if (!next)
            return std::unexpected(next.error());
        owned = std::move(*next);
        current = owned.get();
    }
    return owned;
}

axk::app::Result<void> visit_directory(HANDLE directory, std::string_view relative_path,
                                       const DirectoryVisitor &visitor) {
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64U * 1024U> buffer{};
    for (;;) {
        if (GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo, buffer.data(),
                                         static_cast<DWORD>(buffer.size())) == 0) {
            if (GetLastError() == ERROR_NO_MORE_FILES)
                return {};
            return std::unexpected(reference_error("sandbox directory cannot be enumerated safely", relative_path));
        }
        auto *entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer.data());
        for (;;) {
            const std::wstring_view native_name{entry->FileName, entry->FileNameLength / sizeof(wchar_t)};
            if (native_name != L"." && native_name != L".." &&
                (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U) {
                const std::filesystem::path name{native_name};
                const auto utf8_name = axk::text::path_to_utf8(name);
                if (entry_name_from_utf8(utf8_name)) {
                    NativeDirectoryEntry discovered{.name = name,
                                                    .utf8_name = utf8_name,
                                                    .kind = axk::app::DirectoryEntryKind::file,
                                                    .size = std::nullopt};
                    bool supported = true;
                    if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                        discovered.kind = axk::app::DirectoryEntryKind::directory;
                    } else if (entry->EndOfFile.QuadPart >= 0) {
                        discovered.size = static_cast<std::uint64_t>(entry->EndOfFile.QuadPart);
                    } else {
                        supported = false;
                    }
                    if (supported) {
                        auto proceed = visitor(discovered);
                        if (!proceed)
                            return std::unexpected(proceed.error());
                        if (!*proceed)
                            return {};
                    }
                }
            }
            if (entry->NextEntryOffset == 0U)
                break;
            entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(reinterpret_cast<std::byte *>(entry) +
                                                              entry->NextEntryOffset);
        }
    }
}

axk::app::Result<std::vector<std::byte>> read_prefix(HANDLE directory, const NativeDirectoryEntry &entry,
                                                     std::size_t maximum_bytes, std::string_view relative_path) {
    const auto prefix_size = static_cast<std::size_t>(std::min<std::uint64_t>(entry.size.value_or(0U), maximum_bytes));
    std::vector<std::byte> prefix(prefix_size);
    if (prefix.empty())
        return prefix;
    auto opened = open_relative(directory, entry.name, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN,
                                FILE_NON_DIRECTORY_FILE, relative_path);
    if (!opened)
        return std::unexpected(opened.error());
    DWORD read{};
    if (ReadFile(opened->get(), prefix.data(), static_cast<DWORD>(prefix.size()), &read, nullptr) == 0 ||
        read != static_cast<DWORD>(prefix.size())) {
        return std::unexpected(reference_error("sandbox file prefix cannot be read", relative_path));
    }
    return prefix;
}

#else

NativeHandle descriptor_handle(int descriptor) { return NativeHandle{new int{descriptor}}; }

axk::app::Result<NativeHandle> open_parent(int root, const std::filesystem::path &relative,
                                           std::string_view relative_path) {
    auto descriptor = ::openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return std::unexpected(reference_error("sandbox root handle could not be reopened", relative_path));
    auto current = descriptor_handle(descriptor);
    for (const auto &component : relative) {
        const auto next = ::openat(*current, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0)
            return std::unexpected(
                reference_error("sandbox path contains a link or inaccessible component", relative_path));
        current = descriptor_handle(next);
    }
    return current;
}

axk::app::Result<void> visit_directory(int directory, std::string_view relative_path, const DirectoryVisitor &visitor) {
    const auto enumeration_descriptor = ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (enumeration_descriptor < 0)
        return std::unexpected(reference_error("sandbox directory cannot be reopened", relative_path));
    auto *stream = ::fdopendir(enumeration_descriptor);
    if (stream == nullptr) {
        ::close(enumeration_descriptor);
        return std::unexpected(reference_error("sandbox directory cannot be enumerated safely", relative_path));
    }
    errno = 0;
    while (const auto *entry = ::readdir(stream)) {
        const std::string_view native_name{entry->d_name};
        if (native_name == "." || native_name == "..")
            continue;
        auto name = entry_name_from_utf8(native_name);
        if (!name)
            continue;
        struct stat status{};
        if (::fstatat(directory, name->c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            const auto saved_errno = errno;
            ::closedir(stream);
            errno = saved_errno;
            return std::unexpected(reference_error("sandbox directory changed while it was listed", relative_path));
        }
        if (S_ISLNK(status.st_mode))
            continue;
        NativeDirectoryEntry discovered{.name = *name,
                                        .utf8_name = std::string{native_name},
                                        .kind = axk::app::DirectoryEntryKind::file,
                                        .size = std::nullopt};
        if (S_ISDIR(status.st_mode)) {
            discovered.kind = axk::app::DirectoryEntryKind::directory;
        } else if (S_ISREG(status.st_mode) && status.st_size >= 0) {
            discovered.size = static_cast<std::uint64_t>(status.st_size);
        } else {
            continue;
        }
        auto proceed = visitor(discovered);
        if (!proceed) {
            ::closedir(stream);
            return std::unexpected(proceed.error());
        }
        if (!*proceed) {
            ::closedir(stream);
            return {};
        }
        errno = 0;
    }
    const auto enumeration_error = errno;
    ::closedir(stream);
    if (enumeration_error != 0)
        return std::unexpected(reference_error("sandbox directory cannot be enumerated safely", relative_path));
    return {};
}

axk::app::Result<std::vector<std::byte>> read_prefix(int directory, const NativeDirectoryEntry &entry,
                                                     std::size_t maximum_bytes, std::string_view relative_path) {
    const auto prefix_size = static_cast<std::size_t>(std::min<std::uint64_t>(entry.size.value_or(0U), maximum_bytes));
    std::vector<std::byte> prefix(prefix_size);
    if (prefix.empty())
        return prefix;
    const auto descriptor = ::openat(directory, entry.name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return std::unexpected(reference_error("sandbox file prefix cannot be opened", relative_path));
    auto opened = descriptor_handle(descriptor);
    std::size_t completed{};
    while (completed < prefix.size()) {
        const auto count =
            ::pread(*opened, prefix.data() + completed, prefix.size() - completed, static_cast<off_t>(completed));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::unexpected(reference_error("sandbox file prefix cannot be read", relative_path));
        completed += static_cast<std::size_t>(count);
    }
    return prefix;
}

int rename_no_replace(int source_parent, const char *source_name, int destination_parent,
                      const char *destination_name) {
#if defined(__linux__)
    constexpr unsigned int no_replace = 1U;
    return static_cast<int>(
        ::syscall(SYS_renameat2, source_parent, source_name, destination_parent, destination_name, no_replace));
#elif defined(__APPLE__)
    return ::renameatx_np(source_parent, source_name, destination_parent, destination_name, RENAME_EXCL);
#else
#error "Atomic no-replace rename is required for this platform"
#endif
}

int rename_exchange(int parent, const char *first, const char *second) {
#if defined(__linux__)
    constexpr unsigned int exchange = 2U;
    return static_cast<int>(::syscall(SYS_renameat2, parent, first, parent, second, exchange));
#elif defined(__APPLE__)
    return ::renameatx_np(parent, first, parent, second, RENAME_SWAP);
#else
#error "Atomic exchange rename is required for this platform"
#endif
}

#endif

bool same_file_revision(const NativeIdentity &left, const NativeIdentity &right) noexcept {
#if defined(_WIN32)
    return left.volume_serial == right.volume_serial && left.file_id == right.file_id && left.size == right.size &&
           left.last_write_time == right.last_write_time;
#else
    return left == right;
#endif
}

std::string revision_token(const NativeIdentity &identity) {
    std::ostringstream source;
    source << std::hex << std::setfill('0');
#if defined(_WIN32)
    source << std::setw(16) << identity.volume_serial << ':';
    for (const auto value : identity.file_id)
        source << std::setw(2) << std::to_integer<unsigned int>(value);
    source << ':' << std::setw(16) << identity.size << ':' << std::setw(16) << identity.last_write_time;
#else
    source << std::setw(16) << identity.device << ':' << std::setw(16) << identity.inode << ':' << std::setw(16)
           << identity.size << ':' << std::setw(16) << identity.modification_seconds << ':' << std::setw(16)
           << identity.modification_nanoseconds << ':' << std::setw(16) << identity.change_seconds << ':'
           << std::setw(16) << identity.change_nanoseconds;
#endif
    const auto serialized = source.str();
    SHA256 hash;
    hash.add(serialized.data(), serialized.size());
    return hash.getHash();
}

#if defined(_WIN32)
axk::app::Result<NativeIdentity> native_identity(HANDLE handle, std::string_view relative_path) {
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) == 0 ||
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) == 0 ||
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0 ||
        standard.EndOfFile.QuadPart < 0) {
        return std::unexpected(reference_error("sandbox entry identity cannot be inspected", relative_path));
    }
    NativeIdentity result;
    result.volume_serial = id.VolumeSerialNumber;
    std::memcpy(result.file_id.data(), &id.FileId, sizeof(id.FileId));
    result.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    result.last_write_time = basic.LastWriteTime.QuadPart;
    result.change_time = basic.ChangeTime.QuadPart;
    return result;
}
#else
NativeIdentity native_identity(const struct stat &status) {
#if defined(__APPLE__)
    const auto modification = status.st_mtimespec;
    const auto change = status.st_ctimespec;
#else
    const auto modification = status.st_mtim;
    const auto change = status.st_ctim;
#endif
    return {static_cast<std::uint64_t>(status.st_dev),       static_cast<std::uint64_t>(status.st_ino),
            static_cast<std::uint64_t>(status.st_size),      static_cast<std::int64_t>(modification.tv_sec),
            static_cast<std::int64_t>(modification.tv_nsec), static_cast<std::int64_t>(change.tv_sec),
            static_cast<std::int64_t>(change.tv_nsec)};
}

axk::app::Result<NativeIdentity> native_identity(int descriptor, std::string_view relative_path) {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || status.st_size < 0)
        return std::unexpected(reference_error("sandbox entry identity cannot be inspected", relative_path));
    return native_identity(status);
}
#endif

} // namespace axk::app::filesystem_internal
