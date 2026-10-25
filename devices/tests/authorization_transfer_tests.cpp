#include "posix_authorization.hpp"
#include "test_util.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rawdisk;
using namespace rawdisk::detail;
using namespace std::chrono_literals;

namespace {

void test_closed_peer_does_not_raise_sigpipe() {
    auto sockets = authorization_sockets();
    CHECK(sockets.has_value());
    if (!sockets)
        return;
    auto &[ours, theirs] = *sockets;
    CHECK((::fcntl(ours.get(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(theirs.get(), F_GETFD) & FD_CLOEXEC) != 0);
    {
        PosixFd close_now(std::move(theirs));
    }
    const std::array<std::byte, 32> form{};
    const auto result = send_authorization_bytes(ours.get(), form, std::chrono::steady_clock::now() + 1s);
    CHECK(!result);
    if (!result)
        CHECK(result.error() == std::errc::broken_pipe || result.error() == std::errc::connection_reset);
}

void test_expired_transfer_sends_no_bytes() {
    auto sockets = authorization_sockets();
    CHECK(sockets.has_value());
    if (!sockets)
        return;
    const std::array<std::byte, 32> form{};
    CHECK(err(send_authorization_bytes(sockets->first.get(), form, std::chrono::steady_clock::now() - 1ms)) ==
          Errc::authorization_timeout);
    char byte{};
    CHECK(::recv(sockets->second.get(), &byte, 1, MSG_DONTWAIT) == -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
}

void send_descriptor(int socket) {
    const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        ::_exit(3);
    char byte{};
    iovec iov{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    auto *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &descriptor, sizeof descriptor);
    const auto sent = ::sendmsg(socket, &message, 0);
    ::close(descriptor);
    ::_exit(sent == 1 ? 0 : 3);
}

void test_partial_transfer_and_failed_transfer_reap_child(bool hung) {
    auto sockets = authorization_sockets();
    CHECK(sockets.has_value());
    if (!sockets)
        return;
    auto &[ours, theirs] = *sockets;
    const int capacity = 2048;
    CHECK(::setsockopt(ours.get(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity) == 0);
    const std::vector<std::byte> form(256U * 1024U, std::byte{0x5a});
    const auto pid = ::fork();
    CHECK(pid >= 0);
    if (pid < 0)
        return;
    if (pid == 0) {
        ::close(ours.get());
        if (hung) {
            for (;;)
                ::pause();
        }
        std::array<std::byte, 31> chunk{};
        std::size_t received{};
        while (received < form.size()) {
            const auto count = ::read(theirs.get(), chunk.data(), chunk.size());
            if (count <= 0)
                ::_exit(3);
            for (ssize_t index = 0; index < count; ++index)
                if (chunk[static_cast<std::size_t>(index)] != std::byte{0x5a})
                    ::_exit(3);
            received += static_cast<std::size_t>(count);
        }
        send_descriptor(theirs.get());
    }
    const auto result = [&]() -> Result<PosixFd> {
        AuthorizedChild child(pid);
        {
            PosixFd close_now(std::move(theirs));
        }
        const auto deadline = std::chrono::steady_clock::now() + (hung ? 50ms : 5s);
        const auto sent = send_authorization_bytes(ours.get(), form, deadline);
        if (!sent)
            return std::unexpected(sent.error());
        return receive_authorized_fd(ours.get(), child, Access::ReadOnly, deadline);
    }();
    if (hung)
        CHECK(err(result) == Errc::authorization_timeout);
    else
        CHECK(result.has_value());
    int status{};
    CHECK(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
}

} // namespace

int main() {
    test_closed_peer_does_not_raise_sigpipe();
    test_expired_transfer_sends_no_bytes();
    test_partial_transfer_and_failed_transfer_reap_child(false);
    test_partial_transfer_and_failed_transfer_reap_child(true);
    return finish_tests();
}
