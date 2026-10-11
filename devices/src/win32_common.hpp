#pragma once

#include "detail.hpp"
#include "rawdisk/block_device.hpp"

#include <memory>
#include <string>
#include <utility>

#include <windows.h>

namespace rawdisk::detail {

class RemoteSession;

inline std::error_code win_error(DWORD e) noexcept { return {static_cast<int>(e), std::system_category()}; }
inline std::error_code last_error() noexcept { return win_error(::GetLastError()); }

class Handle {
  public:
    HANDLE h = INVALID_HANDLE_VALUE;

    Handle() noexcept = default;
    explicit Handle(HANDLE v) noexcept : h(v) {}
    ~Handle() { reset(); }
    Handle(Handle &&o) noexcept : h(std::exchange(o.h, INVALID_HANDLE_VALUE)) {}
    Handle &operator=(Handle &&o) noexcept {
        if (this != &o) {
            reset();
            h = std::exchange(o.h, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    bool valid() const noexcept { return h != INVALID_HANDLE_VALUE && h != nullptr; }
    void reset() noexcept {
        if (valid())
            ::CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
};

bool is_elevated() noexcept;
std::pair<std::string, std::string> win32_usb_identity(HANDLE disk);

// Elevated helper session shared by all devices opened through it (win32_elevation.cpp).
Result<std::shared_ptr<RemoteSession>> win32_elevated_session(const ElevationOptions &options);
// The running helper session, or null. Never starts one (no prompt).
std::shared_ptr<RemoteSession> win32_existing_session() noexcept;

} // namespace rawdisk::detail
