#include "filesystem_internal.hpp"

axk::app::Result<axk::app::SandboxTree> axk::app::Sandbox::open_tree(const DirectoryRef &reference,
                                                                     const SandboxTreeLimits &limits) const {
    if (limits.maximum_entries == 0U || limits.maximum_total_file_bytes == 0U || limits.maximum_depth == 0U ||
        limits.maximum_path_bytes == 0U) {
        return std::unexpected(reference_error("directory traversal limits must be positive", reference.relative_path));
    }
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
#if defined(_WIN32)
    auto opened_root = open_parent(root->native->handle, *relative, reference.relative_path);
#else
    auto opened_root = open_parent(root->native->descriptor, *relative, reference.relative_path);
#endif
    if (!opened_root)
        return std::unexpected(opened_root.error());

    struct PendingDirectory {
        std::filesystem::path relative;
        std::optional<NativeIdentity> identity;
        std::size_t depth{};
    };
    std::vector<PendingDirectory> pending;
    pending.push_back({{}, std::nullopt, 0U});
    struct CollectedEntry {
        SandboxTreeEntry entry;
        NativeIdentity identity;
    };
    std::vector<CollectedEntry> collected;
    std::uint64_t total_bytes{};
    std::size_t total_path_bytes{};
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
#if defined(_WIN32)
        auto current_handle = open_parent(opened_root->get(), current.relative, reference.relative_path);
#else
        auto current_handle = open_parent(**opened_root, current.relative, reference.relative_path);
#endif
        if (!current_handle)
            return std::unexpected(current_handle.error());
        if (current.identity) {
#if defined(_WIN32)
            auto identity = native_identity(current_handle->get(), text::path_to_utf8(current.relative));
#else
            auto identity = native_identity(**current_handle, text::path_to_utf8(current.relative));
#endif
            if (!identity || *identity != *current.identity) {
                return std::unexpected(entry_error("archive_source_changed",
                                                   "directory changed while it was being archived",
                                                   text::path_to_utf8(current.relative)));
            }
        }
#if defined(_WIN32)
        alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64U * 1024U> buffer{};
        for (;;) {
            if (GetFileInformationByHandleEx(current_handle->get(), FileIdBothDirectoryInfo, buffer.data(),
                                             static_cast<DWORD>(buffer.size())) == 0) {
                if (GetLastError() == ERROR_NO_MORE_FILES)
                    break;
                return std::unexpected(
                    reference_error("sandbox directory cannot be enumerated safely", reference.relative_path));
            }
            auto *entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer.data());
            for (;;) {
                const std::wstring_view native_name{entry->FileName, entry->FileNameLength / sizeof(wchar_t)};
                if (native_name != L"." && native_name != L"..") {
                    const std::filesystem::path name{native_name};
                    const auto name_utf8 = text::path_to_utf8(name);
                    if (!entry_name_from_utf8(name_utf8))
                        return std::unexpected(
                            reference_error("directory contains a non-portable entry name", reference.relative_path));
                    if ((entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
                        return std::unexpected(
                            reference_error("directory archives do not follow links", reference.relative_path));
                    const auto child_relative = current.relative / name;
                    const auto child_path = text::path_to_utf8(child_relative);
                    const auto child_depth = current.depth + 1U;
                    if (collected.size() >= limits.maximum_entries || child_depth > limits.maximum_depth ||
                        child_path.size() > limits.maximum_path_bytes - total_path_bytes) {
                        return std::unexpected(entry_error("download_archive_too_large",
                                                           "directory archive exceeds configured limits",
                                                           reference.relative_path));
                    }
                    if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                        auto child =
                            open_relative(current_handle->get(), name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                          FILE_OPEN, FILE_DIRECTORY_FILE, reference.relative_path);
                        if (!child)
                            return std::unexpected(child.error());
                        auto identity = native_identity(child->get(), child_path);
                        if (!identity)
                            return std::unexpected(identity.error());
                        total_path_bytes += child_path.size();
                        collected.push_back({{child_path, SandboxTreeEntryKind::directory, 0U}, *identity});
                        pending.push_back({child_relative, *identity, child_depth});
                    } else {
                        auto child = open_relative(current_handle->get(), name, FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                                                   FILE_OPEN, FILE_NON_DIRECTORY_FILE, reference.relative_path);
                        if (!child)
                            return std::unexpected(child.error());
                        auto identity = native_identity(child->get(), child_path);
                        if (!identity)
                            return std::unexpected(identity.error());
                        if (identity->size > limits.maximum_total_file_bytes - total_bytes)
                            return std::unexpected(entry_error("download_archive_too_large",
                                                               "directory archive exceeds configured limits",
                                                               reference.relative_path));
                        total_bytes += identity->size;
                        total_path_bytes += child_path.size();
                        collected.push_back(
                            {{child_path, SandboxTreeEntryKind::file, identity->size}, std::move(*identity)});
                    }
                }
                if (entry->NextEntryOffset == 0U)
                    break;
                entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(reinterpret_cast<std::byte *>(entry) +
                                                                  entry->NextEntryOffset);
            }
        }
#else
        const auto enumeration_descriptor = ::dup(**current_handle);
        if (enumeration_descriptor < 0)
            return std::unexpected(reference_error("sandbox directory cannot be duplicated", reference.relative_path));
        auto *directory = ::fdopendir(enumeration_descriptor);
        if (directory == nullptr) {
            ::close(enumeration_descriptor);
            return std::unexpected(
                reference_error("sandbox directory cannot be enumerated safely", reference.relative_path));
        }
        errno = 0;
        while (const auto *entry = ::readdir(directory)) {
            const std::string_view native_name{entry->d_name};
            if (native_name == "." || native_name == "..")
                continue;
            auto name = entry_name_from_utf8(native_name);
            if (!name) {
                ::closedir(directory);
                return std::unexpected(
                    reference_error("directory contains a non-portable entry name", reference.relative_path));
            }
            struct stat status{};
            if (::fstatat(**current_handle, name->c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
                ::closedir(directory);
                return std::unexpected(
                    reference_error("directory changed while it was opened", reference.relative_path));
            }
            if (S_ISLNK(status.st_mode)) {
                ::closedir(directory);
                return std::unexpected(
                    reference_error("directory archives do not follow links", reference.relative_path));
            }
            const auto child_relative = current.relative / *name;
            const auto child_path = text::path_to_utf8(child_relative);
            const auto child_depth = current.depth + 1U;
            if (collected.size() >= limits.maximum_entries || child_depth > limits.maximum_depth ||
                child_path.size() > limits.maximum_path_bytes - total_path_bytes) {
                ::closedir(directory);
                return std::unexpected(entry_error("download_archive_too_large",
                                                   "directory archive exceeds configured limits",
                                                   reference.relative_path));
            }
            const auto identity = native_identity(status);
            if (S_ISDIR(status.st_mode)) {
                const auto child_descriptor =
                    ::openat(**current_handle, name->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (child_descriptor < 0) {
                    ::closedir(directory);
                    return std::unexpected(
                        reference_error("directory changed while it was opened", reference.relative_path));
                }
                ::close(child_descriptor);
                total_path_bytes += child_path.size();
                collected.push_back({{child_path, SandboxTreeEntryKind::directory, 0U}, identity});
                pending.push_back({child_relative, identity, child_depth});
            } else if (S_ISREG(status.st_mode)) {
                const auto child_descriptor =
                    ::openat(**current_handle, name->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                if (child_descriptor < 0) {
                    ::closedir(directory);
                    return std::unexpected(
                        reference_error("directory changed while it was opened", reference.relative_path));
                }
                auto child = descriptor_handle(child_descriptor);
                struct stat opened_status{};
                if (::fstat(*child, &opened_status) != 0 || !S_ISREG(opened_status.st_mode) ||
                    opened_status.st_size < 0) {
                    ::closedir(directory);
                    return std::unexpected(
                        reference_error("directory contains an unsupported entry", reference.relative_path));
                }
                if (native_identity(opened_status) != identity) {
                    ::closedir(directory);
                    return std::unexpected(entry_error("archive_source_changed",
                                                       "directory entry changed while it was inspected", child_path));
                }
                const auto size = static_cast<std::uint64_t>(opened_status.st_size);
                if (size > limits.maximum_total_file_bytes - total_bytes) {
                    ::closedir(directory);
                    return std::unexpected(entry_error("download_archive_too_large",
                                                       "directory archive exceeds configured limits",
                                                       reference.relative_path));
                }
                total_bytes += size;
                total_path_bytes += child_path.size();
                collected.push_back({{child_path, SandboxTreeEntryKind::file, size}, identity});
            } else {
                ::closedir(directory);
                return std::unexpected(
                    reference_error("directory contains an unsupported entry", reference.relative_path));
            }
        }
        const auto enumeration_error = errno;
        ::closedir(directory);
        if (enumeration_error != 0)
            return std::unexpected(
                reference_error("sandbox directory cannot be enumerated safely", reference.relative_path));
#endif
    }
    std::ranges::sort(collected, {}, [](const auto &value) { return value.entry.relative_path; });
    auto implementation = std::make_unique<SandboxTree::Implementation>();
    implementation->root = std::move(*opened_root);
    implementation->entries.reserve(collected.size());
    implementation->identities.reserve(collected.size());
    for (auto &value : collected) {
        implementation->entries.push_back(std::move(value.entry));
        implementation->identities.push_back(std::move(value.identity));
    }
    return SandboxTree{std::move(implementation)};
}
