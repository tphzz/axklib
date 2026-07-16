#include "axklib/server/process_lifetime.hpp"

#include <limits>

#ifdef _WIN32
#define NOMINMAX
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
