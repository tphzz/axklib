#pragma once

#include "posix_fd.hpp"
#include "rawdisk/block_device.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <utility>

namespace rawdisk::detail {

using AuthorizationDeadline = std::chrono::steady_clock::time_point;
class AuthorizedChild {
  public:
    explicit AuthorizedChild(pid_t pid) noexcept : pid_(pid) {}
    ~AuthorizedChild();
    AuthorizedChild(const AuthorizedChild &) = delete;
    AuthorizedChild &operator=(const AuthorizedChild &) = delete;
    bool valid() const noexcept { return pid_ > 0; }
    Result<int> wait(AuthorizationDeadline deadline);

  private:
    pid_t pid_;
};

// Private close-on-exec sockets: parent is nonblocking, child stays blocking for authopen.
Result<std::pair<PosixFd, PosixFd>> authorization_sockets();
Result<void> send_authorization_bytes(int socket, std::span<const std::byte> bytes, AuthorizationDeadline deadline);
Result<PosixFd> validate_authorized_fd(PosixFd fd, Access access);
// The caller owns the child before sending authorization data and through final reaping.
Result<PosixFd> receive_authorized_fd(int socket, AuthorizedChild &child, Access access, AuthorizationDeadline deadline,
                                      decltype(&::recvmsg) receive_message = &::recvmsg);
#if defined(__APPLE__)
using AuthorizedOpenProcess =
    std::function<Result<PosixFd>(const std::string &, Access, std::span<const std::byte>, AuthorizationDeadline)>;
Result<PosixFd> run_macos_authorization_process(const std::string &path, Access access, std::span<const std::byte> form,
                                                AuthorizationDeadline deadline, const std::string &executable);
Result<PosixFd> macos_authorized_open(const std::string &path, Access access, AuthorizationSession &authorization,
                                      AuthorizationDeadline deadline = std::chrono::steady_clock::now() +
                                                                       std::chrono::seconds(120),
                                      const AuthorizedOpenProcess &process = {});
#endif

} // namespace rawdisk::detail
