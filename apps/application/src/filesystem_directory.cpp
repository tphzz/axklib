#include "filesystem_internal.hpp"

axk::app::Result<axk::app::EntryMetadata> axk::app::Sandbox::metadata(std::string_view root_id,
                                                                      std::string_view relative_path) const {
    const auto root = find_root(root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", relative_path));
    auto resolved = resolve_existing(root_id, relative_path);
    if (!resolved)
        return std::unexpected(resolved.error());
    std::error_code error;
    const auto status = std::filesystem::status(*resolved, error);
    if (error)
        return std::unexpected(reference_error("sandbox entry cannot be inspected", relative_path));
    EntryMetadata result{.root_id = std::string{root_id},
                         .relative_path = std::string{relative_path},
                         .kind = DirectoryEntryKind::file,
                         .size = std::nullopt,
                         .writable = root->info.writable};
    if (std::filesystem::is_directory(status)) {
        result.kind = DirectoryEntryKind::directory;
    } else if (std::filesystem::is_regular_file(status)) {
        result.size = std::filesystem::file_size(*resolved, error);
        if (error)
            return std::unexpected(reference_error("sandbox file size cannot be inspected", relative_path));
    } else {
        return std::unexpected(reference_error("sandbox entry type is unsupported", relative_path));
    }
    return result;
}

axk::app::Result<axk::app::DirectoryListing>
axk::app::Sandbox::list_directory(const DirectoryRef &reference, std::size_t limit,
                                  std::optional<std::string_view> cursor) const {
    if (limit == 0U || limit > 5000U)
        return std::unexpected(
            reference_error("directory listing limit must be between 1 and 5000", reference.relative_path));
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
#if defined(_WIN32)
    auto directory = open_parent(root->native->handle, *relative, reference.relative_path);
#else
    auto directory = open_parent(root->native->descriptor, *relative, reference.relative_path);
#endif
    if (!directory)
        return std::unexpected(directory.error());

    std::optional<std::string> after_key;
    if (cursor) {
        auto decoded = decode_cursor(*cursor, reference.relative_path);
        if (!decoded)
            return std::unexpected(decoded.error());
        after_key = std::move(*decoded);
    }

    DirectoryListing result{.directory = reference, .entries = {}, .truncated = false, .next_cursor = std::nullopt};
    const auto collect = [&](const NativeDirectoryEntry &discovered) -> Result<bool> {
        const auto entry_relative = reference.relative_path.empty()
                                        ? discovered.utf8_name
                                        : reference.relative_path + '/' + discovered.utf8_name;
        DirectoryEntry entry{.name = discovered.utf8_name,
                             .relative_path = entry_relative,
                             .kind = discovered.kind,
                             .size = discovered.size};
        const auto key = entry_key(entry);
        if (after_key && key <= *after_key)
            return true;
        const auto position = std::ranges::lower_bound(
            result.entries, entry,
            [](const DirectoryEntry &left, const DirectoryEntry &right) { return entry_key(left) < entry_key(right); });
        result.entries.insert(position, std::move(entry));
        if (result.entries.size() > limit + 1U)
            result.entries.pop_back();
        return true;
    };
#if defined(_WIN32)
    auto visited = visit_directory(directory->get(), reference.relative_path, collect);
#else
    auto visited = visit_directory(**directory, reference.relative_path, collect);
#endif
    if (!visited)
        return std::unexpected(visited.error());
    if (result.entries.size() > limit) {
        result.entries.pop_back();
        result.truncated = true;
        result.next_cursor = encode_cursor(entry_key(result.entries.back()));
    }
    return result;
}

axk::app::Result<axk::app::MediaSourceInspection>
axk::app::Sandbox::inspect_media_source(const DirectoryRef &reference) const {
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
#if defined(_WIN32)
    auto directory = open_parent(root->native->handle, *relative, reference.relative_path);
#else
    auto directory = open_parent(root->native->descriptor, *relative, reference.relative_path);
#endif
    if (!directory)
        return std::unexpected(directory.error());

    MediaSourceInspection result;
    std::vector<std::filesystem::path> children;
    bool unsupported{};
    const auto inspect_entry = [&](const NativeDirectoryEntry &entry, auto directory_handle,
                                   bool nested) -> Result<bool> {
        ++result.entries_visited;
        if (result.entries_visited > axk::AxkObjectDirectory::maximum_entries) {
            unsupported = true;
            return false;
        }
        if (entry.kind == DirectoryEntryKind::directory) {
            if (nested) {
                unsupported = true;
                return false;
            }
            children.push_back(entry.name);
            return true;
        }
        constexpr std::size_t object_prefix_size = 12U;
        constexpr std::size_t segment_prefix_size = 0x28U;
        auto prefix = read_prefix(directory_handle, entry, nested ? segment_prefix_size : object_prefix_size,
                                  reference.relative_path);
        if (!prefix)
            return std::unexpected(prefix.error());
        ++result.prefixes_read;
        if (axk::AxkObjectDirectory::recognizes_entry_prefix(*prefix, nested)) {
            result.kind = DirectoryMediaSourceKind::axk_object_directory;
            return false;
        }
        return true;
    };

    std::size_t root_entries{};
    const auto inspect_root = [&](const NativeDirectoryEntry &entry) -> Result<bool> {
        if (++root_entries > axk::AxkObjectDirectory::maximum_leaf_entries) {
            unsupported = true;
            return false;
        }
#if defined(_WIN32)
        return inspect_entry(entry, directory->get(), false);
#else
        return inspect_entry(entry, **directory, false);
#endif
    };
#if defined(_WIN32)
    auto inspected = visit_directory(directory->get(), reference.relative_path, inspect_root);
#else
    auto inspected = visit_directory(**directory, reference.relative_path, inspect_root);
#endif
    if (!inspected)
        return std::unexpected(inspected.error());
    if (result.kind || unsupported)
        return result;

    for (const auto &child_name : children) {
#if defined(_WIN32)
        auto child = open_relative(directory->get(), child_name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN,
                                   FILE_DIRECTORY_FILE, reference.relative_path);
#else
        const auto descriptor =
            ::openat(**directory, child_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        auto child =
            descriptor >= 0
                ? Result<NativeHandle>{descriptor_handle(descriptor)}
                : std::unexpected(reference_error("sandbox child directory cannot be opened", reference.relative_path));
#endif
        if (!child)
            return std::unexpected(child.error());
        std::size_t leaf_entries{};
        const auto inspect_child = [&](const NativeDirectoryEntry &entry) -> Result<bool> {
            if (++leaf_entries > axk::AxkObjectDirectory::maximum_leaf_entries) {
                unsupported = true;
                return false;
            }
#if defined(_WIN32)
            return inspect_entry(entry, child->get(), true);
#else
            return inspect_entry(entry, **child, true);
#endif
        };
#if defined(_WIN32)
        inspected = visit_directory(child->get(), reference.relative_path, inspect_child);
#else
        inspected = visit_directory(**child, reference.relative_path, inspect_child);
#endif
        if (!inspected)
            return std::unexpected(inspected.error());
        if (result.kind || unsupported)
            return result;
    }
    return result;
}
