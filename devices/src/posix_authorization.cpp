#include "posix_authorization.hpp"
#include "detail.hpp"

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
namespace {

class Child {
  public:
    explicit Child(pid_t pid) : pid_(pid) {}
    ~Child() {
        if (pid_ <= 0)
            return;
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    Result<int> wait(AuthorizationDeadline deadline) {
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

  private:
    pid_t pid_;
};

} // namespace

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

Result<PosixFd> receive_authorized_fd(int socket, pid_t child, Access access, AuthorizationDeadline deadline) {
    if (child <= 0)
        return fail(Errc::invalid_argument);
    Child process(child);
    std::vector<PosixFd> descriptors;
    descriptors.reserve(16U);
    char byte{};
    iovec iov{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)]{};
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
        received = ::recvmsg(socket, &message, MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received < 0)
            return fail(errno_code(errno));
        break;
    }
    bool malformed = (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0;
    for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len < CMSG_LEN(0)) {
            malformed = true;
            continue;
        }
        const auto bytes = header->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0U)
            malformed = true;
        for (std::size_t offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int)) {
            int descriptor = -1;
            std::memcpy(&descriptor, CMSG_DATA(header) + offset, sizeof descriptor);
            descriptors.emplace_back(descriptor);
        }
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
