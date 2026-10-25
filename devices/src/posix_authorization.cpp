#include "posix_authorization.hpp"
#include "detail.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

namespace rawdisk::detail {
constexpr std::size_t authorization_descriptor_capacity = 512U;

AuthorizedChild::~AuthorizedChild() {
    if (pid_ <= 0)
        return;
    ::kill(pid_, SIGKILL);
    while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
    }
}
Result<int> AuthorizedChild::wait(AuthorizationDeadline deadline) {
    if (!valid())
        return fail(Errc::invalid_argument);
    for (;;) {
        int status{};
        const auto result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return status;
        }
        if (result < 0 && errno != EINTR) {
            if (errno == ECHILD)
                pid_ = -1;
            return fail(Errc::elevation_failed);
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return fail(Errc::authorization_timeout);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Result<std::pair<PosixFd, PosixFd>> authorization_sockets() {
    int sockets[2];
#if defined(__linux__)
    constexpr int flags = SOCK_STREAM | SOCK_CLOEXEC;
#else
    constexpr int flags = SOCK_STREAM;
#endif
    if (::socketpair(AF_UNIX, flags, 0, sockets) != 0)
        return fail(errno_code(errno));
    PosixFd original_ours(sockets[0]);
    PosixFd original_theirs(sockets[1]);
    if (::fcntl(original_ours.get(), F_SETFD, FD_CLOEXEC) != 0 ||
        ::fcntl(original_theirs.get(), F_SETFD, FD_CLOEXEC) != 0)
        return fail(errno_code(errno));
    PosixFd ours(::fcntl(original_ours.get(), F_DUPFD_CLOEXEC, 3));
    if (!ours.valid())
        return fail(errno_code(errno));
    PosixFd theirs(::fcntl(original_theirs.get(), F_DUPFD_CLOEXEC, 3));
    if (!theirs.valid())
        return fail(errno_code(errno));
    // Darwin's full-buffer send path does not honor MSG_DONTWAIT. Keep only our endpoint nonblocking.
    const int status = ::fcntl(ours.get(), F_GETFL);
    if (status < 0 || ::fcntl(ours.get(), F_SETFL, status | O_NONBLOCK) != 0)
        return fail(errno_code(errno));
#if defined(__APPLE__)
    const int enabled = 1;
    if (::setsockopt(ours.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) != 0 ||
        ::setsockopt(theirs.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) != 0)
        return fail(errno_code(errno));
#endif
    return std::pair(std::move(ours), std::move(theirs));
}

Result<void> send_authorization_bytes(int socket, std::span<const std::byte> bytes, AuthorizationDeadline deadline) {
    std::size_t done{};
    while (done < bytes.size()) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            return fail(Errc::authorization_timeout);
        pollfd event{socket, POLLOUT, 0};
        const auto ready = ::poll(&event, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return fail(errno_code(errno));
        if (ready == 0)
            return fail(Errc::authorization_timeout);
#if defined(__linux__)
        constexpr int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
        constexpr int flags = MSG_DONTWAIT;
#endif
        const auto count = ::send(socket, bytes.data() + done, bytes.size() - done, flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count < 0)
            return fail(errno_code(errno));
        if (count == 0)
            return fail(Errc::elevation_failed);
        done += static_cast<std::size_t>(count);
    }
    return {};
}

Result<PosixFd> validate_authorized_fd(PosixFd fd, Access access) {
    if (!fd.valid())
        return fail(Errc::elevation_failed);
    const int flags = ::fcntl(fd.get(), F_GETFL);
    if (flags < 0)
        return fail(errno_code(errno));
    if ((flags & O_ACCMODE) != (access == Access::ReadWrite ? O_RDWR : O_RDONLY))
        return fail(Errc::helper_protocol);
    if (::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) != 0)
        return fail(errno_code(errno));
    return fd;
}

Result<PosixFd> receive_authorized_fd(int socket, AuthorizedChild &process, Access access,
                                      AuthorizationDeadline deadline, decltype(&::recvmsg) receive_message) {
    if (!process.valid())
        return fail(Errc::invalid_argument);
    std::vector<PosixFd> descriptors;
    descriptors.reserve(authorization_descriptor_capacity);
    char byte{};
    iovec iov{&byte, 1};
    // XNU installs rights before copying control data out; reserve its full FD limit.
    // https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/uipc_usrreq.c
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * authorization_descriptor_capacity)]{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    ssize_t received{};
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            return fail(Errc::authorization_timeout);
        pollfd event{socket, POLLIN, 0};
        const int polled = ::poll(&event, 1, static_cast<int>(remaining.count()));
        if (polled < 0 && errno == EINTR)
            continue;
        if (polled < 0)
            return fail(errno_code(errno));
        if (polled == 0)
            return fail(Errc::authorization_timeout);
        message.msg_controllen = sizeof control;
        message.msg_flags = 0;
        received = receive_message(socket, &message, MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received < 0)
            return fail(errno_code(errno));
        break;
    }
    bool malformed = (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0 || message.msg_controllen > sizeof control;
    const auto control_size = std::min<std::size_t>(message.msg_controllen, sizeof control);
    for (std::size_t offset = 0; control_size - offset >= CMSG_LEN(0);) {
        auto *header = reinterpret_cast<cmsghdr *>(control + offset);
        const auto length = static_cast<std::size_t>(header->cmsg_len);
        if (length < CMSG_LEN(0)) {
            malformed = true;
            break;
        }
        const auto remaining = control_size - offset;
        const auto bytes = std::min(length, remaining) - CMSG_LEN(0);
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
            malformed = true;
        } else {
            if (bytes % sizeof(int) != 0U)
                malformed = true;
            for (std::size_t index = 0; index + sizeof(int) <= bytes; index += sizeof(int)) {
                int descriptor = -1;
                std::memcpy(&descriptor, CMSG_DATA(header) + index, sizeof descriptor);
                descriptors.emplace_back(descriptor);
            }
        }
        if (length > remaining) {
            malformed = true;
            break;
        }
        const auto advance = CMSG_SPACE(length - CMSG_LEN(0));
        if (advance > remaining)
            break;
        offset += advance;
    }
    if (malformed || descriptors.size() > 1U)
        return fail(Errc::helper_protocol);
    auto status = process.wait(deadline);
    if (!status)
        return fail(status.error());
    if (descriptors.empty()) {
        if (WIFEXITED(*status) && WEXITSTATUS(*status) == 1)
            return fail(Errc::elevation_cancelled);
        return fail(Errc::elevation_failed);
    }
    if (received != 1 || !WIFEXITED(*status) || WEXITSTATUS(*status) != 0)
        return fail(Errc::elevation_failed);
    return validate_authorized_fd(std::move(descriptors.front()), access);
}

} // namespace rawdisk::detail
