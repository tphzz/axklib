#pragma once

#include "posix_fd.hpp"
#include "rawdisk/block_device.hpp"

#include <chrono>
#include <string>
#include <sys/types.h>

namespace rawdisk::detail {

using AuthorizationDeadline = std::chrono::steady_clock::time_point;
Result<PosixFd> validate_authorized_fd(PosixFd fd, Access access);
// Takes ownership of child and always reaps it, including malformed replies and timeouts.
Result<PosixFd> receive_authorized_fd(int socket, pid_t child, Access access, AuthorizationDeadline deadline);
#if defined(__APPLE__)
Result<PosixFd> macos_authorized_open(const std::string &path, Access access);
#endif

} // namespace rawdisk::detail
