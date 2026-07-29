#include "filesystem_internal.hpp"

axk::app::Result<axk::app::EntryMetadata> axk::app::Sandbox::create_directory(const DirectoryRef &parent,
                                                                              std::string_view name) const {
    const std::scoped_lock mutation_lock{state_->mutation_mutex};
    const auto root = find_root(parent.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", parent.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", parent.relative_path));
    auto relative_parent = relative_path_from_utf8(parent.relative_path);
    if (!relative_parent)
        return std::unexpected(relative_parent.error());
    auto filename = entry_name_from_utf8(name);
    if (!filename)
        return std::unexpected(filename.error());

    const auto relative_path =
        parent.relative_path.empty() ? std::string{name} : parent.relative_path + '/' + std::string{name};
#if defined(_WIN32)
    auto directory = open_parent(root->native->handle, *relative_parent, parent.relative_path);
    if (!directory)
        return std::unexpected(directory.error());
    auto created = open_relative(directory->get(), *filename, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
                                 FILE_CREATE, FILE_DIRECTORY_FILE, relative_path);
    if (!created) {
        auto existing = open_relative(directory->get(), *filename, FILE_READ_ATTRIBUTES, FILE_OPEN, 0U, relative_path);
        if (existing)
            return std::unexpected(output_exists_error("sandbox entry already exists", relative_path));
        return std::unexpected(created.error());
    }
#else
    auto directory = open_parent(root->native->descriptor, *relative_parent, parent.relative_path);
    if (!directory)
        return std::unexpected(directory.error());
    if (::mkdirat(**directory, filename->c_str(), 0777) != 0) {
        if (errno == EEXIST)
            return std::unexpected(output_exists_error("sandbox entry already exists", relative_path));
        return std::unexpected(
            entry_error("entry_mutation_failed", "sandbox directory could not be created", relative_path));
    }
#endif
    return EntryMetadata{.root_id = parent.root_id,
                         .relative_path = relative_path,
                         .kind = DirectoryEntryKind::directory,
                         .size = std::nullopt,
                         .writable = true};
}

axk::app::Result<axk::app::EntryMetadata> axk::app::Sandbox::rename_entry(const FileRef &reference,
                                                                          std::string_view name) const {
    const std::scoped_lock mutation_lock{state_->mutation_mutex};
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("sandbox roots cannot be renamed", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    auto filename = entry_name_from_utf8(name);
    if (!filename)
        return std::unexpected(filename.error());

    const auto parent_relative = relative->parent_path().generic_string();
    const auto relative_path = parent_relative.empty() ? std::string{name} : parent_relative + '/' + std::string{name};
#if defined(_WIN32)
    auto directory = open_parent(root->native->handle, relative->parent_path(), reference.relative_path);
    if (!directory)
        return std::unexpected(directory.error());
    auto destination = open_relative(directory->get(), *filename, FILE_READ_ATTRIBUTES, FILE_OPEN, 0U, relative_path);
    if (destination)
        return std::unexpected(output_exists_error("sandbox entry already exists", relative_path));
    auto source = open_relative(directory->get(), relative->filename(), DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, 0U,
                                reference.relative_path);
    if (!source)
        return std::unexpected(source.error());
    FILE_STANDARD_INFO source_information{};
    if (GetFileInformationByHandleEx(source->get(), FileStandardInfo, &source_information,
                                     sizeof(source_information)) == 0) {
        return std::unexpected(reference_error("sandbox entry cannot be inspected", reference.relative_path));
    }
    auto result = EntryMetadata{
        .root_id = reference.root_id,
        .relative_path = relative_path,
        .kind = source_information.Directory ? DirectoryEntryKind::directory : DirectoryEntryKind::file,
        .size = source_information.Directory
                    ? std::nullopt
                    : std::optional<std::uintmax_t>{static_cast<std::uintmax_t>(source_information.EndOfFile.QuadPart)},
        .writable = true};
    if (rename_open_entry(source->get(), directory->get(), *filename, false) < 0)
        return std::unexpected(
            entry_error("entry_mutation_failed", "sandbox entry could not be renamed", reference.relative_path));
#else
    auto directory = open_parent(root->native->descriptor, relative->parent_path(), reference.relative_path);
    if (!directory)
        return std::unexpected(directory.error());
    struct stat source_status{};
    if (::fstatat(**directory, relative->filename().c_str(), &source_status, AT_SYMLINK_NOFOLLOW) != 0 ||
        S_ISLNK(source_status.st_mode)) {
        return std::unexpected(
            reference_error("sandbox entry is a link or cannot be inspected", reference.relative_path));
    }
    if (!S_ISDIR(source_status.st_mode) && !S_ISREG(source_status.st_mode))
        return std::unexpected(reference_error("sandbox entry type is unsupported", reference.relative_path));
    auto result =
        EntryMetadata{.root_id = reference.root_id,
                      .relative_path = relative_path,
                      .kind = S_ISDIR(source_status.st_mode) ? DirectoryEntryKind::directory : DirectoryEntryKind::file,
                      .size = S_ISREG(source_status.st_mode)
                                  ? std::optional<std::uintmax_t>{static_cast<std::uintmax_t>(source_status.st_size)}
                                  : std::nullopt,
                      .writable = true};
    struct stat destination_status{};
    if (::fstatat(**directory, filename->c_str(), &destination_status, AT_SYMLINK_NOFOLLOW) == 0)
        return std::unexpected(output_exists_error("sandbox entry already exists", relative_path));
    if (errno != ENOENT)
        return std::unexpected(
            entry_error("entry_mutation_failed", "rename target cannot be inspected", relative_path));
    if (rename_no_replace(**directory, relative->filename().c_str(), **directory, filename->c_str()) != 0) {
        if (errno == EEXIST)
            return std::unexpected(output_exists_error("sandbox entry already exists", relative_path));
        return std::unexpected(
            entry_error("entry_mutation_failed", "sandbox entry could not be renamed", reference.relative_path));
    }
#endif
    return result;
}

axk::app::Result<void> axk::app::Sandbox::delete_entry(const FileRef &reference) const {
    const std::scoped_lock mutation_lock{state_->mutation_mutex};
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("sandbox roots cannot be deleted", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
#if defined(_WIN32)
    auto directory = open_parent(root->native->handle, relative->parent_path(), reference.relative_path);
    if (!directory)
        return std::unexpected(directory.error());
    auto entry = open_relative(directory->get(), relative->filename(), DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, 0U,
                               reference.relative_path);
    if (!entry)
        return std::unexpected(entry.error());
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    if (SetFileInformationByHandle(entry->get(), FileDispositionInfo, &disposition, sizeof(disposition)) == 0) {
        const auto error = GetLastError();
        if (error == ERROR_DIR_NOT_EMPTY)
            return std::unexpected(
                entry_error("directory_not_empty", "only empty directories can be deleted", reference.relative_path));
        return std::unexpected(
            entry_error("entry_mutation_failed", "sandbox entry could not be deleted", reference.relative_path));
    }
#else
    auto directory = open_parent(root->native->descriptor, relative->parent_path(), reference.relative_path);
    if (!directory)
        return std::unexpected(directory.error());
    struct stat status{};
    if (::fstatat(**directory, relative->filename().c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        S_ISLNK(status.st_mode)) {
        return std::unexpected(
            reference_error("sandbox entry is a link or cannot be inspected", reference.relative_path));
    }
    const auto flags = S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0;
    if (::unlinkat(**directory, relative->filename().c_str(), flags) != 0) {
        if (errno == ENOTEMPTY || errno == EEXIST)
            return std::unexpected(
                entry_error("directory_not_empty", "only empty directories can be deleted", reference.relative_path));
        return std::unexpected(
            entry_error("entry_mutation_failed", "sandbox entry could not be deleted", reference.relative_path));
    }
#endif
    return {};
}

axk::app::Result<void> axk::app::Sandbox::require_distinct(const FileRef &source, const FileRef &destination) const {
    auto source_path = resolve_file(source);
    if (!source_path)
        return std::unexpected(source_path.error());
    const auto root = find_root(destination.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", destination.relative_path));
    auto relative = relative_path_from_utf8(destination.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    std::error_code error;
    const auto destination_path = std::filesystem::weakly_canonical(root->canonical_path / *relative, error);
    if (error)
        return std::unexpected(reference_error("destination path cannot be resolved", destination.relative_path));
    const auto destination_exists = std::filesystem::exists(destination_path, error);
    if (error)
        return std::unexpected(reference_error("destination path cannot be inspected", destination.relative_path));
    const auto same_file = destination_exists && std::filesystem::equivalent(*source_path, destination_path, error);
    if (error)
        return std::unexpected(reference_error("destination path cannot be compared", destination.relative_path));
    if (*source_path == destination_path || same_file)
        return std::unexpected(
            reference_error("source and destination must be different files", destination.relative_path));
    return {};
}

axk::app::Result<std::size_t> axk::app::Sandbox::cleanup_abandoned_publications() const {
    // Publication candidates are not authenticated workspace entries. Retain
    // unknown files rather than infer deletion authority from their names.
    return 0U;
}

std::string_view axk::app::directory_entry_kind_name(DirectoryEntryKind kind) noexcept {
    switch (kind) {
    case DirectoryEntryKind::file:
        return "FILE";
    case DirectoryEntryKind::directory:
        return "DIRECTORY";
    }
    return "UNKNOWN";
}

std::string_view axk::app::directory_media_source_kind_name(DirectoryMediaSourceKind kind) noexcept {
    switch (kind) {
    case DirectoryMediaSourceKind::axk_object_directory:
        return "AXK_OBJECT_DIRECTORY";
    }
    return "UNKNOWN";
}
