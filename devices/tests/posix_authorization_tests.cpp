#include "posix_authorization.hpp"
#include "test_util.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rawdisk;
using namespace rawdisk::detail;

namespace {

int descriptor_count() {
    int count{};
    for (int fd = 0; fd < 1024; ++fd)
        if (::fcntl(fd, F_GETFD) >= 0)
            ++count;
    return count;
}

enum class Reply {
    success,
    cancelled,
    missing,
    extra,
    truncated,
    wrong_access,
    failed_after_send,
    hung,
    hung_after_send
};

Result<PosixFd> fake_authorization(Reply reply) {
    int sockets[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    PosixFd ours(sockets[0]);
    PosixFd theirs(sockets[1]);
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::close(ours.get());
        if (reply == Reply::cancelled)
            ::_exit(1);
        if (reply == Reply::missing)
            ::_exit(0);
        if (reply == Reply::hung) {
            for (;;)
                ::pause();
        }
        const int fd = ::open("/dev/null", reply == Reply::wrong_access ? O_RDWR : O_RDONLY);
        if (fd < 0)
            ::_exit(3);
        char byte{};
        iovec iov{&byte, 1};
        std::array<int, 32> descriptors{};
        descriptors.fill(fd);
        const std::size_t count = reply == Reply::truncated ? 32U : reply == Reply::extra ? 2U : 1U;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 32)]{};
        msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = static_cast<decltype(message.msg_controllen)>(CMSG_SPACE(count * sizeof(int)));
        auto *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = static_cast<decltype(header->cmsg_len)>(CMSG_LEN(count * sizeof(int)));
        std::memcpy(CMSG_DATA(header), descriptors.data(), count * sizeof(int));
        if (::sendmsg(theirs.get(), &message, 0) != 1)
            ::_exit(3);
        ::close(fd);
        if (reply == Reply::hung_after_send) {
            for (;;)
                ::pause();
        }
        ::_exit(reply == Reply::failed_after_send ? 2 : 0);
    }
    {
        PosixFd close_now(std::move(theirs));
    }
    const auto timeout = reply == Reply::hung || reply == Reply::hung_after_send ? std::chrono::milliseconds(30)
                                                                                 : std::chrono::milliseconds(3000);
    auto result =
        receive_authorized_fd(ours.get(), child, Access::ReadOnly, std::chrono::steady_clock::now() + timeout);
    int status{};
    CHECK(::waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
    return result;
}

} // namespace

int main() {
    const int before = descriptor_count();
    {
        auto result = fake_authorization(Reply::success);
        CHECK(result.has_value());
        if (result)
            CHECK((::fcntl(result->get(), F_GETFD) & FD_CLOEXEC) != 0);
    }
    CHECK(err(fake_authorization(Reply::cancelled)) == Errc::elevation_cancelled);
    CHECK(err(fake_authorization(Reply::missing)) == Errc::elevation_failed);
    CHECK(err(fake_authorization(Reply::extra)) == Errc::helper_protocol);
    CHECK(err(fake_authorization(Reply::truncated)) == Errc::helper_protocol);
    CHECK(err(fake_authorization(Reply::wrong_access)) == Errc::helper_protocol);
    CHECK(err(fake_authorization(Reply::failed_after_send)) == Errc::elevation_failed);
    CHECK(err(fake_authorization(Reply::hung)) == Errc::authorization_timeout);
    CHECK(err(fake_authorization(Reply::hung_after_send)) == Errc::authorization_timeout);
    CHECK(descriptor_count() == before);
#if defined(__APPLE__)
    ElevationOptions options;
    options.allow = false;
    set_elevation_options(options);
    CHECK(err(macos_authorized_open("/dev/test", Access::ReadOnly)) == std::errc::permission_denied);
    set_elevation_options({});
#endif
    return finish_tests();
}
