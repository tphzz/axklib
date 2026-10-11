#pragma once

#include <unistd.h>
#include <utility>

namespace rawdisk::detail {

class PosixFd {
  public:
    explicit PosixFd(int fd = -1) noexcept : fd_(fd) {}
    ~PosixFd() {
        if (valid())
            ::close(fd_);
    }
    PosixFd(PosixFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    PosixFd(const PosixFd &) = delete;
    PosixFd &operator=(const PosixFd &) = delete;
    PosixFd &operator=(PosixFd &&) = delete;
    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

  private:
    int fd_;
};

} // namespace rawdisk::detail
