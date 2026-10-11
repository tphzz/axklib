#include "axklib/server/process_lifetime.hpp"

#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif

bool axk::server::process_is_running(std::uint64_t process_id) noexcept {
    if (process_id == 0U)
        return false;
#ifdef _WIN32
    if (process_id > std::numeric_limits<DWORD>::max())
        return false;
    const auto process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process_id));
    if (process == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const auto state = WaitForSingleObject(process, 0U);
    CloseHandle(process);
    return state == WAIT_TIMEOUT;
#else
    if (process_id > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
        return false;
    errno = 0;
    const auto result = kill(static_cast<pid_t>(process_id), 0);
    return result == 0 || errno == EPERM;
#endif
}

bool axk::server::desktop_shutdown_requested(const std::filesystem::path &connection_file,
                                             std::string_view bearer_token) noexcept {
    if (connection_file.empty() || bearer_token.empty() || bearer_token.size() > 256U)
        return false;
    try {
        auto path = connection_file;
        path.replace_extension("shutdown");
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::file_size(path, error) != bearer_token.size() || error)
            return false;
        std::ifstream file{path, std::ios::binary};
        std::string token(bearer_token.size(), '\0');
        file.read(token.data(), static_cast<std::streamsize>(token.size()));
        return file && token == bearer_token;
    } catch (...) {
        return false;
    }
}
