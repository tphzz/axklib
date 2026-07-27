#include "axklib/server/state_lease.hpp"

#include <algorithm>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include "axklib/utf8.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

axk::app::Error lease_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

std::filesystem::path normalized_lock_path(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return {};
    auto parent = std::filesystem::canonical(path.parent_path(), error);
    if (error)
        return {};
    return parent / path.filename();
}

} // namespace

struct axk::server::StateNamespaceLease::Implementation {
#if defined(_WIN32)
    std::vector<HANDLE> handles;
#else
    std::vector<int> descriptors;
#endif

    ~Implementation() {
#if defined(_WIN32)
        for (const auto handle : handles)
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
#else
        for (const auto descriptor : descriptors) {
            static_cast<void>(::flock(descriptor, LOCK_UN));
            static_cast<void>(::close(descriptor));
        }
#endif
    }
};

axk::server::StateNamespaceLease::StateNamespaceLease() = default;
axk::server::StateNamespaceLease::~StateNamespaceLease() = default;
axk::server::StateNamespaceLease::StateNamespaceLease(StateNamespaceLease &&) noexcept = default;
axk::server::StateNamespaceLease &
axk::server::StateNamespaceLease::operator=(StateNamespaceLease &&) noexcept = default;

axk::server::StateNamespaceLease::StateNamespaceLease(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

axk::app::Result<axk::server::StateNamespaceLease>
axk::server::StateNamespaceLease::acquire(std::vector<std::filesystem::path> lock_files) {
    for (auto &path : lock_files) {
        if (!path.is_absolute())
            return std::unexpected(lease_error("server_state_invalid", "state lock path must be absolute"));
        path = normalized_lock_path(path);
        if (path.empty())
            return std::unexpected(lease_error("server_state_unavailable", "state lock directory cannot be prepared"));
    }
    std::ranges::sort(lock_files);
    lock_files.erase(std::unique(lock_files.begin(), lock_files.end()), lock_files.end());

    auto implementation = std::make_unique<Implementation>();
    for (const auto &path : lock_files) {
#if defined(_WIN32)
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            const auto code = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
                                  ? "server_state_in_use"
                                  : "server_state_unavailable";
            return std::unexpected(lease_error(code, "server state is already owned or cannot be locked: " +
                                                         axk::text::path_to_utf8(path)));
        }
        implementation->handles.push_back(handle);
#else
        const auto descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (descriptor < 0) {
            return std::unexpected(lease_error("server_state_unavailable",
                                               "server state lock cannot be opened: " + axk::text::path_to_utf8(path)));
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const auto error = errno;
            static_cast<void>(::close(descriptor));
            const auto code =
                error == EWOULDBLOCK || error == EAGAIN ? "server_state_in_use" : "server_state_unavailable";
            return std::unexpected(lease_error(code, "server state is already owned or cannot be locked: " +
                                                         axk::text::path_to_utf8(path)));
        }
        implementation->descriptors.push_back(descriptor);
#endif
    }
    return StateNamespaceLease{std::move(implementation)};
}
