#pragma once

#include "detail.hpp"
#include "rawdisk/block_device.hpp"
#include "win32_identity.hpp"

#include <functional>
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
Result<Win32UsbIdentity> win32_usb_identity(HANDLE disk);
Result<void> win32_verify_usb_identity(const Win32UsbIdentity &bound);

using HelperSessionLauncher = std::function<Result<std::shared_ptr<RemoteSession>>(const ElevationOptions &)>;
Result<std::shared_ptr<RemoteSession>> win32_elevated_session(AuthorizationSession &authorization,
                                                              const HelperSessionLauncher &launcher = {});

} // namespace rawdisk::detail
