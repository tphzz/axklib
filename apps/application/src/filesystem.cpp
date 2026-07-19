#include "axklib/application/filesystem.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iterator>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif
#include <unistd.h>
#endif

#include "axklib/utf8.hpp"

namespace {

axk::app::Error root_error(std::string message) { return {"invalid_sandbox_root", std::move(message)}; }

axk::app::Error reference_error(std::string message, std::string_view relative_path) {
    axk::app::ErrorContext context;
    if (!relative_path.empty())
        context.relative_path = relative_path;
    return {"invalid_file_reference", std::move(message), std::move(context)};
}

axk::app::Error output_exists_error(std::string message, std::string_view relative_path) {
    axk::app::ErrorContext context;
    if (!relative_path.empty())
        context.relative_path = relative_path;
    return {"output_exists", std::move(message), std::move(context)};
}

axk::app::Error entry_error(std::string code, std::string message, std::string_view relative_path) {
    axk::app::ErrorContext context;
    if (!relative_path.empty())
        context.relative_path = relative_path;
    return {std::move(code), std::move(message), std::move(context)};
}

bool valid_root_id(std::string_view value) {
    return !value.empty() && value.size() <= 64U && std::ranges::all_of(value, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
    });
}

bool valid_portable_component(std::string_view value) {
    if (value.empty() || value.ends_with('.') || value.ends_with(' ') ||
        std::ranges::any_of(value, [](unsigned char character) { return character < 0x20U; })) {
        return false;
    }
    const auto dot = value.find('.');
    auto basename = std::string{value.substr(0U, dot)};
    std::ranges::transform(basename, basename.begin(),
                           [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    if (basename == "CON" || basename == "PRN" || basename == "AUX" || basename == "NUL" || basename == "CLOCK$") {
        return false;
    }
    return !(basename.size() == 4U && (basename.starts_with("COM") || basename.starts_with("LPT")) &&
             basename[3] >= '1' && basename[3] <= '9');
}

bool within(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() && candidate != root)
        return false;
    if (relative.is_absolute())
        return false;
    const auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

axk::app::Result<void> verify_no_link_components(const std::filesystem::path &root,
                                                 const std::filesystem::path &relative,
                                                 std::string_view relative_path) {
#if defined(_WIN32)
    auto current = root;
    const auto verify = [&](const std::filesystem::path &path) -> axk::app::Result<void> {
        const auto handle =
            CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return std::unexpected(reference_error("sandbox path contains an inaccessible component", relative_path));
        }
        BY_HANDLE_FILE_INFORMATION information{};
        const auto inspected = GetFileInformationByHandle(handle, &information) != 0;
        CloseHandle(handle);
        if (!inspected || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return std::unexpected(reference_error("sandbox path contains a link component", relative_path));
        }
        return {};
    };
    if (auto verified = verify(current); !verified)
        return verified;
    for (const auto &component : relative) {
        current /= component;
        if (auto verified = verify(current); !verified)
            return verified;
    }
    return {};
#else
    auto descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return std::unexpected(reference_error("sandbox root cannot be opened safely", relative_path));
    for (auto iterator = relative.begin(); iterator != relative.end(); ++iterator) {
        const auto last = std::next(iterator) == relative.end();
        const auto flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? 0 : O_DIRECTORY);
        const auto next = ::openat(descriptor, iterator->c_str(), flags);
        ::close(descriptor);
        descriptor = next;
        if (descriptor < 0)
            return std::unexpected(
                reference_error("sandbox path contains a link or inaccessible component", relative_path));
    }
    ::close(descriptor);
    return {};
#endif
}

std::optional<std::uint64_t> publication_owner_process(const std::filesystem::path &path) {
    const auto name = axk::text::path_to_utf8(path.filename());
    constexpr std::string_view marker{".axklib-publication.p"};
    const auto marker_offset = name.rfind(marker);
    if (marker_offset == std::string::npos || marker_offset == 0U || !name.ends_with(".tmp"))
        return std::nullopt;
    const auto process_begin = marker_offset + marker.size();
    const auto process_end = name.find('.', process_begin);
    if (process_end == std::string::npos || process_end == process_begin)
        return std::nullopt;
    const auto sequence_end = name.find('.', process_end + 1U);
    if (sequence_end == std::string::npos || sequence_end == process_end + 1U || name.substr(sequence_end) != ".tmp") {
        return std::nullopt;
    }
    std::uint64_t process{};
    std::uint64_t sequence{};
    const auto process_text = std::string_view{name}.substr(process_begin, process_end - process_begin);
    const auto sequence_text = std::string_view{name}.substr(process_end + 1U, sequence_end - process_end - 1U);
    const auto [process_tail, process_error] =
        std::from_chars(process_text.data(), process_text.data() + process_text.size(), process);
    const auto [sequence_tail, sequence_error] =
        std::from_chars(sequence_text.data(), sequence_text.data() + sequence_text.size(), sequence);
    if (process_error != std::errc{} || process_tail != process_text.data() + process_text.size() || process == 0U ||
        sequence_error != std::errc{} || sequence_tail != sequence_text.data() + sequence_text.size() ||
        sequence == 0U) {
        return std::nullopt;
    }
    return process;
}

bool process_is_active(std::uint64_t process) {
#if defined(_WIN32)
    if (process > std::numeric_limits<DWORD>::max())
        return false;
    const auto handle =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process));
    if (handle == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const auto state = WaitForSingleObject(handle, 0U);
    CloseHandle(handle);
    return state == WAIT_TIMEOUT;
#else
    if (process > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
        return false;
    if (::kill(static_cast<pid_t>(process), 0) == 0)
        return true;
    return errno == EPERM;
#endif
}

std::string entry_key(const axk::app::DirectoryEntry &entry) {
    return std::string{entry.kind == axk::app::DirectoryEntryKind::directory ? "0\0" : "1\0", 2U} + entry.name;
}

std::string encode_cursor(std::string_view value) {
    constexpr char digits[]{"0123456789abcdef"};
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const auto byte : value) {
        const auto unsigned_byte = static_cast<unsigned char>(byte);
        encoded.push_back(digits[unsigned_byte >> 4U]);
        encoded.push_back(digits[unsigned_byte & 0x0fU]);
    }
    return encoded;
}

axk::app::Result<std::string> decode_cursor(std::string_view value, std::string_view relative_path) {
    if (value.empty() || value.size() % 2U != 0U || value.size() > 2048U)
        return std::unexpected(reference_error("directory cursor is invalid", relative_path));
    std::string decoded;
    decoded.reserve(value.size() / 2U);
    for (std::size_t offset = 0; offset < value.size(); offset += 2U) {
        unsigned int byte{};
        const auto *begin = value.data() + offset;
        const auto [end, error] = std::from_chars(begin, begin + 2U, byte, 16);
        if (error != std::errc{} || end != begin + 2U)
            return std::unexpected(reference_error("directory cursor is invalid", relative_path));
        decoded.push_back(static_cast<char>(byte));
    }
    if (decoded.size() < 3U || (decoded[0] != '0' && decoded[0] != '1') || decoded[1] != '\0')
        return std::unexpected(reference_error("directory cursor is invalid", relative_path));
    return decoded;
}

axk::app::Result<std::filesystem::path> relative_path_from_utf8(std::string_view value) {
    if (value.find('\\') != std::string_view::npos || value.find(':') != std::string_view::npos ||
        value.starts_with('/') || value.ends_with('/') || value.find("//") != std::string_view::npos) {
        return std::unexpected(reference_error("relative path is not normalized", value));
    }
    auto path = axk::text::path_from_utf8(value);
    if (!path)
        return std::unexpected(reference_error("relative path is not valid UTF-8", value));
    if (path->is_absolute() || path->has_root_name() || path->has_root_directory())
        return std::unexpected(reference_error("absolute paths are not permitted", value));
    for (const auto &component : *path) {
        if (component == "." || component == "..")
            return std::unexpected(reference_error("relative path traversal is not permitted", value));
        const auto component_utf8 = axk::text::path_to_utf8(component);
        if (!valid_portable_component(component_utf8))
            return std::unexpected(reference_error("relative path contains a reserved component", value));
    }
    return *path;
}

axk::app::Result<std::filesystem::path> entry_name_from_utf8(std::string_view value) {
    auto name = relative_path_from_utf8(value);
    if (!name)
        return std::unexpected(name.error());
    if (name->empty() || name->has_parent_path() || name->filename() != *name)
        return std::unexpected(reference_error("entry name must be one portable path component", value));
    return name;
}

#if defined(_WIN32)

struct NativeHandleCloser {
    void operator()(void *handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using NativeHandle = std::unique_ptr<void, NativeHandleCloser>;

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

axk::app::Result<NativeHandle> open_parent(HANDLE root, const std::filesystem::path &relative,
                                           std::string_view relative_path) {
    HANDLE current = root;
    NativeHandle owned;
    for (const auto &component : relative) {
        auto next = open_relative(current, component, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN,
                                  FILE_DIRECTORY_FILE, relative_path);
        if (!next)
            return std::unexpected(next.error());
        owned = std::move(*next);
        current = owned.get();
    }
    if (!owned) {
        if (DuplicateHandle(GetCurrentProcess(), root, GetCurrentProcess(), &current, 0U, FALSE,
                            DUPLICATE_SAME_ACCESS) == 0) {
            return std::unexpected(reference_error("sandbox root handle could not be duplicated", relative_path));
        }
        owned.reset(current);
    }
    return owned;
}

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

NativeHandle descriptor_handle(int descriptor) { return NativeHandle{new int{descriptor}}; }

axk::app::Result<NativeHandle> open_parent(int root, const std::filesystem::path &relative,
                                           std::string_view relative_path) {
    auto descriptor = ::dup(root);
    if (descriptor < 0)
        return std::unexpected(reference_error("sandbox root handle could not be duplicated", relative_path));
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

#endif

} // namespace

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

axk::app::Result<std::vector<axk::app::Sandbox::Root>>
axk::app::Sandbox::validate_roots(std::vector<RootDefinition> definitions) {
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

axk::app::Result<axk::app::Sandbox> axk::app::Sandbox::create(std::vector<RootDefinition> definitions) {
    auto roots = validate_roots(std::move(definitions));
    if (!roots)
        return std::unexpected(roots.error());
    return Sandbox{std::move(*roots)};
}

axk::app::Result<void> axk::app::Sandbox::replace_roots(std::vector<RootDefinition> definitions) {
    auto roots = validate_roots(std::move(definitions));
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

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_existing(std::string_view root_id,
                                                                            std::string_view relative_path) const {
    const auto root = find_root(root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", relative_path));
    auto relative = relative_path_from_utf8(relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    if (auto verified = verify_no_link_components(root->canonical_path, *relative, relative_path); !verified)
        return std::unexpected(verified.error());

    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(root->canonical_path / *relative, error);
    if (error || !std::filesystem::exists(candidate, error) || error)
        return std::unexpected(entry_error("entry_not_found", "sandbox entry does not exist", relative_path));
    if (!within(root->canonical_path, candidate))
        return std::unexpected(reference_error("sandbox entry escapes its configured root", relative_path));
    return candidate;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_file(const FileRef &reference) const {
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("file reference requires a relative path", reference.relative_path));
    auto result = resolve_existing(reference.root_id, reference.relative_path);
    if (!result)
        return result;
    std::error_code error;
    if (!std::filesystem::is_regular_file(*result, error) || error)
        return std::unexpected(reference_error("file reference does not name a regular file", reference.relative_path));
    return result;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_directory(const DirectoryRef &reference) const {
    auto result = resolve_existing(reference.root_id, reference.relative_path);
    if (!result)
        return result;
    std::error_code error;
    if (!std::filesystem::is_directory(*result, error) || error)
        return std::unexpected(
            reference_error("directory reference does not name a directory", reference.relative_path));
    return result;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_output_file(const FileRef &reference,
                                                                               bool overwrite) const {
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (!root->info.writable)
        return std::unexpected(reference_error("sandbox root is read-only", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("output file requires a relative path", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    if (relative->filename().empty())
        return std::unexpected(reference_error("output file requires a filename", reference.relative_path));

    if (auto verified =
            verify_no_link_components(root->canonical_path, relative->parent_path(), reference.relative_path);
        !verified) {
        return std::unexpected(verified.error());
    }

    std::error_code error;
    const auto parent = std::filesystem::canonical(root->canonical_path / relative->parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) || error || !within(root->canonical_path, parent))
        return std::unexpected(reference_error("output parent is not a sandbox directory", reference.relative_path));
    const auto candidate = parent / relative->filename();
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return std::unexpected(reference_error("output path cannot be inspected", reference.relative_path));
    if (!error && std::filesystem::exists(status)) {
        if (!overwrite)
            return std::unexpected(output_exists_error("output file already exists", reference.relative_path));
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
            return std::unexpected(reference_error("output path is not a regular file", reference.relative_path));
    }
    return candidate;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_output_directory(const DirectoryRef &reference,
                                                                                    bool overwrite) const {
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (!root->info.writable)
        return std::unexpected(reference_error("sandbox root is read-only", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("output directory requires a relative path", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    if (relative->filename().empty()) {
        return std::unexpected(reference_error("output directory requires a name", reference.relative_path));
    }
    if (auto verified =
            verify_no_link_components(root->canonical_path, relative->parent_path(), reference.relative_path);
        !verified) {
        return std::unexpected(verified.error());
    }
    std::error_code error;
    const auto parent = std::filesystem::canonical(root->canonical_path / relative->parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) || error || !within(root->canonical_path, parent)) {
        return std::unexpected(reference_error("output parent is not a sandbox directory", reference.relative_path));
    }
    const auto candidate = parent / relative->filename();
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return std::unexpected(reference_error("output path cannot be inspected", reference.relative_path));
    if (!error && std::filesystem::exists(status)) {
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            return std::unexpected(reference_error("output path is not a directory", reference.relative_path));
        }
        if (!overwrite) {
            const auto empty = std::filesystem::is_empty(candidate, error);
            if (error || !empty)
                return std::unexpected(
                    output_exists_error("output directory already exists and is not empty", reference.relative_path));
        }
    }
    return candidate;
}

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
    if (limit == 0U || limit > 1000U)
        return std::unexpected(
            reference_error("directory listing limit must be between 1 and 1000", reference.relative_path));
    auto directory = resolve_directory(reference);
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
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{*directory, error}, end; !error && iterator != end;
         iterator.increment(error)) {
        const auto name = text::path_to_utf8(iterator->path().filename());
        const auto relative = reference.relative_path.empty() ? name : reference.relative_path + '/' + name;
        const auto resolved = resolve_existing(reference.root_id, relative);
        if (!resolved)
            continue;
        const auto status = std::filesystem::status(*resolved, error);
        if (error)
            break;
        DirectoryEntry entry{
            .name = name, .relative_path = relative, .kind = DirectoryEntryKind::file, .size = std::nullopt};
        if (std::filesystem::is_directory(status)) {
            entry.kind = DirectoryEntryKind::directory;
        } else if (std::filesystem::is_regular_file(status)) {
            entry.kind = DirectoryEntryKind::file;
            entry.size = std::filesystem::file_size(*resolved, error);
            if (error)
                break;
        } else {
            continue;
        }
        const auto key = entry_key(entry);
        if (after_key && key <= *after_key)
            continue;
        const auto position = std::ranges::lower_bound(
            result.entries, entry,
            [](const DirectoryEntry &left, const DirectoryEntry &right) { return entry_key(left) < entry_key(right); });
        result.entries.insert(position, std::move(entry));
        if (result.entries.size() > limit + 1U)
            result.entries.pop_back();
    }
    if (error)
        return std::unexpected(reference_error("directory cannot be listed", reference.relative_path));
    if (result.entries.size() > limit) {
        result.entries.pop_back();
        result.truncated = true;
        result.next_cursor = encode_cursor(entry_key(result.entries.back()));
    }
    return result;
}

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
    const auto destination_name = filename->native();
    const auto bytes = sizeof(FILE_RENAME_INFO) + destination_name.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(bytes);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = directory->get();
    rename->FileNameLength = static_cast<DWORD>(destination_name.size() * sizeof(wchar_t));
    std::copy(destination_name.begin(), destination_name.end(), rename->FileName);
    if (SetFileInformationByHandle(source->get(), FileRenameInfo, rename, static_cast<DWORD>(bytes)) == 0)
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
    std::vector<std::filesystem::path> abandoned;
    const auto roots = [this] {
        const std::shared_lock lock{state_->mutex};
        return state_->roots;
    }();
    for (const auto &root : roots) {
        if (!root.info.writable)
            continue;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator
                 iterator{root.canonical_path, std::filesystem::directory_options::skip_permission_denied, error},
             end;
             !error && iterator != end; iterator.increment(error)) {
            const auto status = iterator->symlink_status(error);
            if (error)
                break;
            if (std::filesystem::is_symlink(status)) {
                if (iterator->is_directory(error))
                    iterator.disable_recursion_pending();
                error.clear();
                continue;
            }
            const auto process = publication_owner_process(iterator->path());
            if (process && !process_is_active(*process) &&
                (std::filesystem::is_regular_file(status) || std::filesystem::is_directory(status))) {
                if (std::filesystem::is_directory(status))
                    iterator.disable_recursion_pending();
                abandoned.push_back(iterator->path());
            }
        }
        if (error)
            return std::unexpected(root_error("sandbox publication cleanup could not enumerate a writable root"));
    }
    std::ranges::sort(abandoned, [](const auto &left, const auto &right) {
        return std::distance(left.begin(), left.end()) > std::distance(right.begin(), right.end());
    });
    std::size_t removed{};
    for (const auto &path : abandoned) {
        std::error_code error;
        const auto count = std::filesystem::remove_all(path, error);
        if (error)
            return std::unexpected(root_error("sandbox publication cleanup could not remove an abandoned output"));
        if (count != 0U)
            ++removed;
    }
    return removed;
}

std::string_view axk::app::directory_entry_kind_name(DirectoryEntryKind kind) noexcept {
    switch (kind) {
    case DirectoryEntryKind::file:
        return "file";
    case DirectoryEntryKind::directory:
        return "directory";
    }
    return "unknown";
}
