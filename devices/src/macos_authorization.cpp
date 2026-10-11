#include "detail.hpp"
#include "posix_authorization.hpp"

#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>

extern char **environ;

namespace rawdisk::detail {
namespace {

Result<std::pair<PosixFd, PosixFd>> authorization_sockets() {
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return fail(errno_code(errno));
    PosixFd original_ours(sockets[0]);
    PosixFd original_theirs(sockets[1]);
    PosixFd ours(::fcntl(original_ours.get(), F_DUPFD_CLOEXEC, 3));
    if (!ours.valid())
        return fail(errno_code(errno));
    PosixFd theirs(::fcntl(original_theirs.get(), F_DUPFD_CLOEXEC, 3));
    if (!theirs.valid())
        return fail(errno_code(errno));
    return std::pair(std::move(ours), std::move(theirs));
}

} // namespace

Result<PosixFd> macos_authorized_open(const std::string &path, Access access) {
    if (!elevation_options().allow)
        return fail(std::make_error_code(std::errc::permission_denied));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto sockets = authorization_sockets();
    if (!sockets)
        return fail(sockets.error());
    auto &[ours, theirs] = *sockets;
    const std::string flags = std::to_string(access == Access::ReadWrite ? O_RDWR : O_RDONLY);
    const char *argv[]{"/usr/libexec/authopen", "-stdoutpipe", "-o", flags.c_str(), path.c_str(), nullptr};
    posix_spawn_file_actions_t actions;
    int result = ::posix_spawn_file_actions_init(&actions);
    if (result != 0)
        return fail(errno_code(result));
    result = ::posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDOUT_FILENO);
    if (result == 0)
        result = ::posix_spawn_file_actions_addclose(&actions, ours.get());
    if (result == 0 && theirs.get() != STDOUT_FILENO)
        result = ::posix_spawn_file_actions_addclose(&actions, theirs.get());
    pid_t child{};
    if (result == 0)
        result = ::posix_spawn(&child, argv[0], &actions, nullptr, const_cast<char *const *>(argv), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    if (result != 0)
        return fail(errno_code(result));
    {
        PosixFd close_now(std::move(theirs));
    }
    return receive_authorized_fd(ours.get(), child, access, deadline);
}

} // namespace rawdisk::detail
