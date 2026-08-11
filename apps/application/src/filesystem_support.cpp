#include "filesystem_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
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
#include <cerrno>
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

#include "axklib/media.hpp"
#include "axklib/utf8.hpp"

namespace axk::app::filesystem_internal {

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

} // namespace axk::app::filesystem_internal
