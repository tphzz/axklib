#include "filesystem_internal.hpp"

#include <unordered_set>

axk::app::Result<std::vector<axk::app::Sandbox::Root>>
axk::app::Sandbox::validate_roots(std::vector<RootDefinition> definitions,
                                  std::span<const std::filesystem::path> protected_paths) {
    std::vector<Root> roots;
    roots.reserve(definitions.size());
    std::unordered_set<std::string> identifiers;
    for (auto &definition : definitions) {
        if (!valid_root_id(definition.id))
            return std::unexpected(root_error("sandbox root ID must use 1-64 letters, digits, '.', '_' or '-'"));
        if (!identifiers.insert(definition.id).second)
            return std::unexpected(root_error("sandbox root IDs must be unique"));
        if (definition.display_name.empty())
            definition.display_name = definition.id;

        std::error_code error;
        const auto canonical = std::filesystem::canonical(definition.path, error);
        if (error || !std::filesystem::is_directory(canonical, error) || error)
            return std::unexpected(root_error("sandbox root must name an existing directory"));
        if (std::ranges::any_of(protected_paths, [&canonical](const auto &protected_path) {
                return within(canonical, protected_path) || within(protected_path, canonical);
            })) {
            return std::unexpected(root_error("sandbox root overlaps protected application state"));
        }
#if defined(_WIN32)
        const auto handle = CreateFileW(canonical.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::unexpected(root_error("sandbox root cannot be opened safely"));
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            CloseHandle(handle);
            return std::unexpected(root_error("sandbox root must not be a reparse point"));
        }
        auto native = std::make_shared<NativeRoot>(handle);
#else
        const auto descriptor = ::open(canonical.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0)
            return std::unexpected(root_error("sandbox root cannot be opened safely"));
        auto native = std::make_shared<NativeRoot>(descriptor);
#endif
        roots.push_back({{std::move(definition.id), std::move(definition.display_name), definition.writable},
                         canonical,
                         std::move(native)});
    }
    return roots;
}

axk::app::Result<axk::app::Sandbox> axk::app::Sandbox::create(std::vector<RootDefinition> definitions,
                                                              std::vector<std::filesystem::path> protected_paths) {
    for (auto &path : protected_paths) {
        std::error_code error;
        path = std::filesystem::weakly_canonical(path, error);
        if (error)
            return std::unexpected(root_error("protected application path cannot be canonicalized"));
    }
    auto roots = validate_roots(std::move(definitions), protected_paths);
    if (!roots)
        return std::unexpected(roots.error());
    return Sandbox{std::move(*roots), std::move(protected_paths)};
}

axk::app::Result<void> axk::app::Sandbox::replace_roots(std::vector<RootDefinition> definitions) {
    auto roots = validate_roots(std::move(definitions), state_->protected_paths);
    if (!roots)
        return std::unexpected(roots.error());
    const std::unique_lock lock{state_->mutex};
    state_->roots = std::move(*roots);
    return {};
}

std::vector<axk::app::RootInfo> axk::app::Sandbox::roots() const {
    const std::shared_lock lock{state_->mutex};
    std::vector<RootInfo> result;
    result.reserve(state_->roots.size());
    for (const auto &root : state_->roots)
        result.push_back(root.info);
    return result;
}

std::optional<axk::app::Sandbox::Root> axk::app::Sandbox::find_root(std::string_view root_id) const {
    const std::shared_lock lock{state_->mutex};
    const auto found =
        std::ranges::find(state_->roots, root_id, [](const Root &root) { return std::string_view{root.info.id}; });
    return found == state_->roots.end() ? std::nullopt : std::optional<Root>{*found};
}

axk::app::Result<axk::app::SandboxFile> axk::app::Sandbox::open_file(const FileRef &reference) const {
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("file reference requires a relative path", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    auto parent =
#if defined(_WIN32)
        open_parent(root->native->handle, relative->parent_path(), reference.relative_path);
#else
        open_parent(root->native->descriptor, relative->parent_path(), reference.relative_path);
#endif
    if (!parent)
        return std::unexpected(parent.error());

#if defined(_WIN32)
    auto handle = open_relative(parent->get(), relative->filename(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN,
                                FILE_NON_DIRECTORY_FILE, reference.relative_path);
    if (!handle)
        return std::unexpected(handle.error());
    auto identity = native_identity(handle->get(), reference.relative_path);
#else
    const auto descriptor =
        ::openat(**parent, relative->filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return std::unexpected(reference_error("sandbox file cannot be opened safely", reference.relative_path));
    auto handle = descriptor_handle(descriptor);
    struct stat status{};
    if (::fstat(*handle, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        return std::unexpected(reference_error("file reference does not name a regular file", reference.relative_path));
    }
    auto identity = native_identity(*handle, reference.relative_path);
#endif
    if (!identity)
        return std::unexpected(identity.error());
    const auto size = identity->size;
    const auto filename = text::path_to_utf8(relative->filename());
#if defined(_WIN32)
    auto reader = std::make_shared<NativeFileReader>(std::move(*handle), size, reference.relative_path);
#else
    auto reader = std::make_shared<NativeFileReader>(std::move(handle), size, reference.relative_path);
#endif
    const auto expected = *identity;
    return SandboxFile{reference, filename,
                       size,      revision_token(expected),
                       reader,    [reader, expected] { return reader->verify_unchanged(expected); }};
}

axk::app::Result<std::shared_ptr<axk::app::SandboxMutation>>
axk::app::Sandbox::open_mutation(const FileRef &reference) const {
    auto mutation_lock = std::unique_lock{state_->mutation_mutex};
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("file reference requires a relative path", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative || relative->filename().empty())
        return std::unexpected(relative ? reference_error("file reference requires a filename", reference.relative_path)
                                        : relative.error());
    auto parent =
#if defined(_WIN32)
        open_parent(root->native->handle, relative->parent_path(), reference.relative_path);
#else
        open_parent(root->native->descriptor, relative->parent_path(), reference.relative_path);
#endif
    if (!parent)
        return std::unexpected(parent.error());

#if defined(_WIN32)
    auto file =
        open_relative(parent->get(), relative->filename(), FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                      FILE_OPEN, FILE_NON_DIRECTORY_FILE, reference.relative_path);
    if (!file)
        return std::unexpected(file.error());
    auto identity = native_identity(file->get(), reference.relative_path);
#else
    const auto descriptor =
        ::openat(**parent, relative->filename().c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox file cannot be opened for mutation",
                                           reference.relative_path));
    auto file = descriptor_handle(descriptor);
    struct stat status{};
    if (::fstat(*file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
        return std::unexpected(
            entry_error("entry_mutation_failed", "mutation target is not a regular file", reference.relative_path));
    auto identity = native_identity(*file, reference.relative_path);
#endif
    if (!identity)
        return std::unexpected(identity.error());
    auto implementation = std::make_unique<SandboxMutation::Implementation>();
    implementation->reference = reference;
    implementation->filename = relative->filename();
    implementation->parent = std::move(*parent);
#if defined(_WIN32)
    implementation->file = std::move(*file);
#else
    implementation->file = std::move(file);
#endif
    implementation->identity = *identity;
    implementation->size = identity->size;
    implementation->mutation_lock = std::move(mutation_lock);
    return std::shared_ptr<SandboxMutation>{new SandboxMutation{std::move(implementation)}};
}
