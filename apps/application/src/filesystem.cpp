#include "axklib/application/filesystem.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <format>
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

axk::app::Error publication_error(std::string message, std::string_view relative_path) {
    axk::app::ErrorContext context;
    context.relative_path = relative_path;
    return {"output_publication_failed", std::move(message), std::move(context)};
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

  private:
    NativeHandle handle_;
    std::uint64_t size_{};
    std::string source_name_;
    mutable std::mutex mutex_;
};

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

bool rename_open_entry(HANDLE entry, HANDLE parent, const std::filesystem::path &name, bool replace) {
    const auto native = name.native();
    const auto bytes = sizeof(FILE_RENAME_INFO) + native.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(bytes);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = replace ? TRUE : FALSE;
    rename->RootDirectory = parent;
    rename->FileNameLength = static_cast<DWORD>(native.size() * sizeof(wchar_t));
    std::copy(native.begin(), native.end(), rename->FileName);
    return SetFileInformationByHandle(entry, FileRenameInfo, rename, static_cast<DWORD>(bytes)) != 0;
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

bool delete_tree_at(int parent, const std::filesystem::path &name) {
    const auto descriptor = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    auto *directory = ::fdopendir(descriptor);
    if (directory == nullptr) {
        ::close(descriptor);
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
            if (!delete_tree_at(descriptor, entry->d_name)) {
                removed = false;
                break;
            }
        } else if (!S_ISREG(status.st_mode) || ::unlinkat(descriptor, entry->d_name, 0) != 0) {
            removed = false;
            break;
        }
    }
    const auto enumeration_error = errno;
    ::closedir(directory);
    return removed && enumeration_error == 0 && ::unlinkat(parent, name.c_str(), AT_REMOVEDIR) == 0;
}

std::optional<bool> directory_empty_at(int parent, const std::filesystem::path &name) {
    const auto descriptor = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return std::nullopt;
    auto *directory = ::fdopendir(descriptor);
    if (directory == nullptr) {
        ::close(descriptor);
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
    if (enumeration_error != 0)
        return std::nullopt;
    return empty;
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
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(handle->get(), &file_size) == 0 || file_size.QuadPart < 0)
        return std::unexpected(reference_error("sandbox file size cannot be inspected", reference.relative_path));
    const auto size = static_cast<std::uint64_t>(file_size.QuadPart);
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
    const auto size = static_cast<std::uint64_t>(status.st_size);
#endif
    const auto filename = text::path_to_utf8(relative->filename());
#if defined(_WIN32)
    auto reader = std::make_shared<NativeFileReader>(std::move(*handle), size, reference.relative_path);
#else
    auto reader = std::make_shared<NativeFileReader>(std::move(handle), size, reference.relative_path);
#endif
    return SandboxFile{reference, filename, size, std::move(reader)};
}

axk::app::Result<std::vector<axk::app::SandboxTreeFile>>
axk::app::Sandbox::open_tree_files(const DirectoryRef &reference, std::size_t maximum_entries,
                                   std::uint64_t maximum_total_bytes) const {
    if (maximum_entries == 0U || maximum_total_bytes == 0U)
        return std::unexpected(reference_error("directory traversal limits must be positive", reference.relative_path));
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
        NativeHandle handle;
        std::filesystem::path relative;
    };
    std::vector<PendingDirectory> pending;
    pending.push_back({std::move(*opened_root), {}});
    std::vector<SandboxTreeFile> result;
    std::uint64_t total_bytes{};
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
#if defined(_WIN32)
        alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64U * 1024U> buffer{};
        for (;;) {
            if (GetFileInformationByHandleEx(current.handle.get(), FileIdBothDirectoryInfo, buffer.data(),
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
                    if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                        auto child =
                            open_relative(current.handle.get(), name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                          FILE_OPEN, FILE_DIRECTORY_FILE, reference.relative_path);
                        if (!child)
                            return std::unexpected(child.error());
                        pending.push_back({std::move(*child), child_relative});
                    } else {
                        auto child = open_relative(current.handle.get(), name, FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                                                   FILE_OPEN, FILE_NON_DIRECTORY_FILE, reference.relative_path);
                        if (!child)
                            return std::unexpected(child.error());
                        FILE_STANDARD_INFO information{};
                        if (GetFileInformationByHandleEx(child->get(), FileStandardInfo, &information,
                                                         sizeof(information)) == 0 ||
                            information.Directory || information.EndOfFile.QuadPart < 0) {
                            return std::unexpected(
                                reference_error("directory contains an unsupported entry", reference.relative_path));
                        }
                        const auto size = static_cast<std::uint64_t>(information.EndOfFile.QuadPart);
                        if (result.size() >= maximum_entries || size > maximum_total_bytes - total_bytes)
                            return std::unexpected(entry_error("download_archive_too_large",
                                                               "directory archive exceeds configured limits",
                                                               reference.relative_path));
                        total_bytes += size;
                        auto reader = std::make_shared<NativeFileReader>(std::move(*child), size,
                                                                         text::path_to_utf8(child_relative));
                        result.push_back({text::path_to_utf8(child_relative), size, std::move(reader)});
                    }
                }
                if (entry->NextEntryOffset == 0U)
                    break;
                entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(reinterpret_cast<std::byte *>(entry) +
                                                                  entry->NextEntryOffset);
            }
        }
#else
        const auto enumeration_descriptor = ::dup(*current.handle);
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
            if (::fstatat(*current.handle, name->c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
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
            if (S_ISDIR(status.st_mode)) {
                const auto child_descriptor =
                    ::openat(*current.handle, name->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (child_descriptor < 0) {
                    ::closedir(directory);
                    return std::unexpected(
                        reference_error("directory changed while it was opened", reference.relative_path));
                }
                pending.push_back({descriptor_handle(child_descriptor), child_relative});
            } else if (S_ISREG(status.st_mode)) {
                const auto child_descriptor =
                    ::openat(*current.handle, name->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
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
                const auto size = static_cast<std::uint64_t>(opened_status.st_size);
                if (result.size() >= maximum_entries || size > maximum_total_bytes - total_bytes) {
                    ::closedir(directory);
                    return std::unexpected(entry_error("download_archive_too_large",
                                                       "directory archive exceeds configured limits",
                                                       reference.relative_path));
                }
                total_bytes += size;
                auto reader =
                    std::make_shared<NativeFileReader>(std::move(child), size, text::path_to_utf8(child_relative));
                result.push_back({text::path_to_utf8(child_relative), size, std::move(reader)});
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
    std::ranges::sort(result, {}, &SandboxTreeFile::relative_path);
    return result;
}

axk::app::Result<void> axk::app::Sandbox::publish_file(const FileRef &destination, bool overwrite,
                                                       const axk::RandomAccessReader &source) const {
    const std::scoped_lock mutation_lock{state_->mutation_mutex};
    const auto root = find_root(destination.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", destination.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", destination.relative_path));
    auto relative = relative_path_from_utf8(destination.relative_path);
    if (!relative || relative->filename().empty())
        return std::unexpected(relative ? reference_error("output file requires a filename", destination.relative_path)
                                        : relative.error());
#if defined(_WIN32)
    auto parent = open_parent(root->native->handle, relative->parent_path(), destination.relative_path);
#else
    auto parent = open_parent(root->native->descriptor, relative->parent_path(), destination.relative_path);
#endif
    if (!parent)
        return std::unexpected(parent.error());

    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto temporary = temporary_entry_name(relative->filename());
#if defined(_WIN32)
        auto output = open_relative(parent->get(), temporary, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE,
                                    FILE_CREATE, FILE_NON_DIRECTORY_FILE, destination.relative_path);
        if (!output)
            continue;
        const auto cleanup = [&] {
            FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
            static_cast<void>(
                SetFileInformationByHandle(output->get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        };
        std::vector<std::byte> buffer(static_cast<std::size_t>(
            std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, source.size()))));
        std::uint64_t offset{};
        while (offset < source.size()) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - offset));
            if (const auto read = source.read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                cleanup();
                return std::unexpected(publication_error(read.error().message, destination.relative_path));
            }
            DWORD written{};
            if (WriteFile(output->get(), buffer.data(), static_cast<DWORD>(count), &written, nullptr) == 0 ||
                written != static_cast<DWORD>(count)) {
                cleanup();
                return std::unexpected(
                    publication_error("temporary output could not be written", destination.relative_path));
            }
            offset += count;
        }
        if (FlushFileBuffers(output->get()) == 0) {
            cleanup();
            return std::unexpected(
                publication_error("temporary output could not be flushed", destination.relative_path));
        }
        const auto destination_name = relative->filename().native();
        const auto bytes = sizeof(FILE_RENAME_INFO) + destination_name.size() * sizeof(wchar_t);
        std::vector<std::byte> storage(bytes);
        auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        rename->ReplaceIfExists = overwrite ? TRUE : FALSE;
        rename->RootDirectory = parent->get();
        rename->FileNameLength = static_cast<DWORD>(destination_name.size() * sizeof(wchar_t));
        std::copy(destination_name.begin(), destination_name.end(), rename->FileName);
        if (SetFileInformationByHandle(output->get(), FileRenameInfo, rename, static_cast<DWORD>(bytes)) == 0) {
            const auto error = GetLastError();
            cleanup();
            if (!overwrite && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
                return std::unexpected(output_exists_error("output file already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output could not be published atomically", destination.relative_path));
        }
#else
        const auto descriptor =
            ::openat(**parent, temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST)
                continue;
            return std::unexpected(
                publication_error("temporary output could not be created", destination.relative_path));
        }
        const auto cleanup = [&] { static_cast<void>(::unlinkat(**parent, temporary.c_str(), 0)); };
        std::vector<std::byte> buffer(static_cast<std::size_t>(
            std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, source.size()))));
        std::uint64_t offset{};
        bool failed{};
        while (offset < source.size() && !failed) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - offset));
            if (const auto read = source.read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                ::close(descriptor);
                cleanup();
                return std::unexpected(publication_error(read.error().message, destination.relative_path));
            }
            auto remaining = std::span{buffer}.first(count);
            while (!remaining.empty()) {
                const auto written = ::write(descriptor, remaining.data(), remaining.size());
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0) {
                    failed = true;
                    break;
                }
                remaining = remaining.subspan(static_cast<std::size_t>(written));
            }
            offset += count;
        }
        if (failed || ::fsync(descriptor) != 0) {
            ::close(descriptor);
            cleanup();
            return std::unexpected(
                publication_error("temporary output could not be written and flushed", destination.relative_path));
        }
        ::close(descriptor);
        const auto published =
            overwrite ? ::renameat(**parent, temporary.c_str(), **parent, relative->filename().c_str())
                      : rename_no_replace(**parent, temporary.c_str(), **parent, relative->filename().c_str());
        if (published != 0) {
            const auto error = errno;
            cleanup();
            if (!overwrite && error == EEXIST)
                return std::unexpected(output_exists_error("output file already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output could not be published atomically", destination.relative_path));
        }
        if (::fsync(**parent) != 0)
            return std::unexpected(
                publication_error("published output directory could not be synchronized", destination.relative_path));
#endif
        return {};
    }
    return std::unexpected(
        publication_error("a unique temporary output could not be reserved", destination.relative_path));
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::create_staging_directory(std::string_view purpose) const {
    auto purpose_path = entry_name_from_utf8(purpose);
    if (!purpose_path)
        return std::unexpected(purpose_path.error());
    std::error_code error;
    const auto temporary_root = std::filesystem::temp_directory_path(error);
    if (error)
        return std::unexpected(publication_error("temporary directory is unavailable", purpose));
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto candidate = temporary_root / temporary_entry_name(*purpose_path);
        if (!std::filesystem::create_directory(candidate, error)) {
            if (!error)
                continue;
            return std::unexpected(publication_error("private staging directory could not be created", purpose));
        }
#if !defined(_WIN32)
        std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
        if (error) {
            std::filesystem::remove(candidate, error);
            return std::unexpected(publication_error("private staging directory could not be secured", purpose));
        }
#endif
        return candidate;
    }
    return std::unexpected(publication_error("a unique private staging directory could not be reserved", purpose));
}

axk::app::Result<void> axk::app::Sandbox::publish_directory(const DirectoryRef &destination, bool overwrite,
                                                            const std::filesystem::path &staging) const {
    struct StagedEntry {
        std::filesystem::path source;
        std::filesystem::path relative;
        bool directory{};
    };

    std::error_code error;
    const auto staging_status = std::filesystem::symlink_status(staging, error);
    if (error || !std::filesystem::is_directory(staging_status) || std::filesystem::is_symlink(staging_status)) {
        return std::unexpected(publication_error("staging directory is unavailable", destination.relative_path));
    }
    std::vector<StagedEntry> entries;
    for (std::filesystem::recursive_directory_iterator iterator{staging, error}, end; iterator != end && !error;
         iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error || std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status))) {
            return std::unexpected(
                publication_error("staging directory contains an unsupported entry", destination.relative_path));
        }
        const auto relative = iterator->path().lexically_relative(staging);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
            return std::unexpected(
                publication_error("staging directory contains an invalid path", destination.relative_path));
        }
        entries.push_back({iterator->path(), relative, std::filesystem::is_directory(status)});
    }
    if (error)
        return std::unexpected(
            publication_error("staging directory could not be enumerated", destination.relative_path));
    std::ranges::sort(entries, [](const auto &left, const auto &right) {
        const auto left_depth = static_cast<std::size_t>(std::distance(left.relative.begin(), left.relative.end()));
        const auto right_depth = static_cast<std::size_t>(std::distance(right.relative.begin(), right.relative.end()));
        return std::tie(left_depth, left.relative) < std::tie(right_depth, right.relative);
    });

    const std::scoped_lock mutation_lock{state_->mutation_mutex};
    const auto root = find_root(destination.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", destination.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", destination.relative_path));
    auto relative = relative_path_from_utf8(destination.relative_path);
    if (!relative || relative->filename().empty()) {
        return std::unexpected(relative ? reference_error("output directory requires a name", destination.relative_path)
                                        : relative.error());
    }
#if defined(_WIN32)
    auto parent = open_parent(root->native->handle, relative->parent_path(), destination.relative_path);
#else
    auto parent = open_parent(root->native->descriptor, relative->parent_path(), destination.relative_path);
#endif
    if (!parent)
        return std::unexpected(parent.error());

    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto temporary = temporary_entry_name(relative->filename());
#if defined(_WIN32)
        auto staged = open_relative(parent->get(), temporary,
                                    FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE,
                                    FILE_CREATE, FILE_DIRECTORY_FILE, destination.relative_path);
        if (!staged)
            continue;
        const auto discard_staged = [&] {
            static_cast<void>(delete_open_tree(staged->get(), destination.relative_path));
            FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
            static_cast<void>(
                SetFileInformationByHandle(staged->get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        };
        for (const auto &entry : entries) {
            auto entry_parent = open_parent(staged->get(), entry.relative.parent_path(), destination.relative_path);
            if (!entry_parent) {
                discard_staged();
                return std::unexpected(entry_parent.error());
            }
            if (entry.directory) {
                auto created =
                    open_relative(entry_parent->get(), entry.relative.filename(),
                                  FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE,
                                  FILE_CREATE, FILE_DIRECTORY_FILE, destination.relative_path);
                if (!created) {
                    discard_staged();
                    return std::unexpected(
                        publication_error("staged directory could not be created", destination.relative_path));
                }
                continue;
            }
            auto input = axk::FileReader::open(entry.source);
            auto output = open_relative(entry_parent->get(), entry.relative.filename(),
                                        FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE, FILE_CREATE,
                                        FILE_NON_DIRECTORY_FILE, destination.relative_path);
            if (!input || !output) {
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be opened", destination.relative_path));
            }
            std::vector<std::byte> buffer(static_cast<std::size_t>(
                std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, (*input)->size()))));
            std::uint64_t offset{};
            while (offset < (*input)->size()) {
                const auto count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*input)->size() - offset));
                if (const auto read = (*input)->read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                    discard_staged();
                    return std::unexpected(publication_error(read.error().message, destination.relative_path));
                }
                DWORD written{};
                if (WriteFile(output->get(), buffer.data(), static_cast<DWORD>(count), &written, nullptr) == 0 ||
                    written != static_cast<DWORD>(count)) {
                    discard_staged();
                    return std::unexpected(
                        publication_error("staged output file could not be written", destination.relative_path));
                }
                offset += count;
            }
            if (FlushFileBuffers(output->get()) == 0) {
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be flushed", destination.relative_path));
            }
        }

        auto existing =
            open_relative(parent->get(), relative->filename(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
                          FILE_OPEN, FILE_DIRECTORY_FILE, destination.relative_path);
        if (existing && !overwrite) {
            const auto empty = directory_is_empty(existing->get());
            if (!empty || !*empty) {
                discard_staged();
                return std::unexpected(
                    output_exists_error("output directory already exists", destination.relative_path));
            }
            existing =
                open_relative(parent->get(), relative->filename(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
                              FILE_OPEN, FILE_DIRECTORY_FILE, destination.relative_path);
            if (!existing) {
                discard_staged();
                return std::unexpected(
                    publication_error("output directory changed during publication", destination.relative_path));
            }
        }
        if (!existing) {
            if (!rename_open_entry(staged->get(), parent->get(), relative->filename(), false)) {
                discard_staged();
                return std::unexpected(
                    publication_error("output directory could not be published atomically", destination.relative_path));
            }
            return {};
        }

        const auto backup = temporary_entry_name(relative->filename());
        if (!rename_open_entry(existing->get(), parent->get(), backup, false)) {
            discard_staged();
            return std::unexpected(
                publication_error("existing output directory could not be reserved", destination.relative_path));
        }
        if (!overwrite) {
            auto inspection = open_relative(parent->get(), backup, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                            FILE_OPEN, FILE_DIRECTORY_FILE, destination.relative_path);
            const auto empty = inspection ? directory_is_empty(inspection->get()) : std::nullopt;
            if (!empty || !*empty) {
                static_cast<void>(rename_open_entry(existing->get(), parent->get(), relative->filename(), false));
                discard_staged();
                return std::unexpected(
                    output_exists_error("output directory changed during publication", destination.relative_path));
            }
        }
        if (!rename_open_entry(staged->get(), parent->get(), relative->filename(), false)) {
            static_cast<void>(rename_open_entry(existing->get(), parent->get(), relative->filename(), false));
            discard_staged();
            return std::unexpected(
                publication_error("output directory could not be published atomically", destination.relative_path));
        }
        if (!delete_open_tree(existing->get(), destination.relative_path)) {
            return std::unexpected(
                publication_error("replaced output directory could not be removed", destination.relative_path));
        }
        FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
        if (SetFileInformationByHandle(existing->get(), FileDispositionInfo, &disposition, sizeof(disposition)) == 0) {
            return std::unexpected(
                publication_error("replaced output directory could not be removed", destination.relative_path));
        }
#else
        if (::mkdirat(**parent, temporary.c_str(), 0700) != 0) {
            if (errno == EEXIST)
                continue;
            return std::unexpected(
                publication_error("temporary output directory could not be created", destination.relative_path));
        }
        const auto staged_descriptor =
            ::openat(**parent, temporary.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staged_descriptor < 0) {
            static_cast<void>(::unlinkat(**parent, temporary.c_str(), AT_REMOVEDIR));
            return std::unexpected(
                publication_error("temporary output directory could not be opened", destination.relative_path));
        }
        auto staged = descriptor_handle(staged_descriptor);
        const auto discard_staged = [&] { static_cast<void>(delete_tree_at(**parent, temporary)); };
        for (const auto &entry : entries) {
            auto entry_parent = open_parent(*staged, entry.relative.parent_path(), destination.relative_path);
            if (!entry_parent) {
                staged.reset();
                discard_staged();
                return std::unexpected(entry_parent.error());
            }
            if (entry.directory) {
                if (::mkdirat(**entry_parent, entry.relative.filename().c_str(), 0700) != 0) {
                    staged.reset();
                    discard_staged();
                    return std::unexpected(
                        publication_error("staged directory could not be created", destination.relative_path));
                }
                continue;
            }
            auto input = axk::FileReader::open(entry.source);
            const auto output = ::openat(**entry_parent, entry.relative.filename().c_str(),
                                         O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (!input || output < 0) {
                if (output >= 0)
                    ::close(output);
                staged.reset();
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be opened", destination.relative_path));
            }
            std::vector<std::byte> buffer(static_cast<std::size_t>(
                std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, (*input)->size()))));
            std::uint64_t offset{};
            bool failed{};
            while (offset < (*input)->size() && !failed) {
                const auto count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*input)->size() - offset));
                if (const auto read = (*input)->read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                    failed = true;
                    break;
                }
                auto remaining = std::span{buffer}.first(count);
                while (!remaining.empty()) {
                    const auto written = ::write(output, remaining.data(), remaining.size());
                    if (written < 0 && errno == EINTR)
                        continue;
                    if (written <= 0) {
                        failed = true;
                        break;
                    }
                    remaining = remaining.subspan(static_cast<std::size_t>(written));
                }
                offset += count;
            }
            if (failed || ::fsync(output) != 0) {
                ::close(output);
                staged.reset();
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be written", destination.relative_path));
            }
            ::close(output);
        }
        if (::fsync(*staged) != 0) {
            staged.reset();
            discard_staged();
            return std::unexpected(
                publication_error("temporary output directory could not be synchronized", destination.relative_path));
        }
        staged.reset();

        struct stat destination_status{};
        const auto destination_exists =
            ::fstatat(**parent, relative->filename().c_str(), &destination_status, AT_SYMLINK_NOFOLLOW) == 0;
        if (!destination_exists && errno != ENOENT) {
            discard_staged();
            return std::unexpected(
                publication_error("output directory could not be inspected", destination.relative_path));
        }
        if (destination_exists && (!S_ISDIR(destination_status.st_mode) || S_ISLNK(destination_status.st_mode))) {
            discard_staged();
            return std::unexpected(
                reference_error("output directory is not a regular directory", destination.relative_path));
        }
        if (destination_exists && !overwrite) {
            const auto empty = directory_empty_at(**parent, relative->filename());
            if (!empty || !*empty) {
                discard_staged();
                return std::unexpected(
                    output_exists_error("output directory already exists", destination.relative_path));
            }
        }
        const auto published =
            destination_exists ? rename_exchange(**parent, temporary.c_str(), relative->filename().c_str())
                               : rename_no_replace(**parent, temporary.c_str(), **parent, relative->filename().c_str());
        if (published != 0) {
            const auto publish_error = errno;
            discard_staged();
            if (!overwrite && publish_error == EEXIST)
                return std::unexpected(
                    output_exists_error("output directory already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output directory could not be published atomically", destination.relative_path));
        }
        if (destination_exists && !overwrite) {
            const auto displaced_empty = directory_empty_at(**parent, temporary);
            if (!displaced_empty || !*displaced_empty) {
                static_cast<void>(rename_exchange(**parent, temporary.c_str(), relative->filename().c_str()));
                discard_staged();
                return std::unexpected(
                    output_exists_error("output directory changed during publication", destination.relative_path));
            }
        }
        if (destination_exists && !delete_tree_at(**parent, temporary)) {
            return std::unexpected(
                publication_error("replaced output directory could not be removed", destination.relative_path));
        }
        if (::fsync(**parent) != 0) {
            return std::unexpected(
                publication_error("published output directory could not be synchronized", destination.relative_path));
        }
#endif
        return {};
    }
    return std::unexpected(
        publication_error("a unique temporary output directory could not be reserved", destination.relative_path));
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
