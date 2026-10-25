// On-demand UAC elevation for Windows.
//
// The app runs as a normal user. When opening a device fails with "access denied", rawdisk
// launches axklib_helper.exe with the "runas" verb (one UAC prompt) and talks to it over a
// named pipe using the protocol in remote.hpp. The helper only ever opens USB disks that pass
// the same enumeration and identity checks as a direct open.
//
// Pipe hardening:
//  - random 128-bit name, first-instance-only, one instance, remote clients rejected
//  - DACL: SYSTEM, Administrators and the launching user only
//  - server accepts only the client whose PID is the process it launched
//  - helper accepts only a server whose PID was passed on its command line, and connects
//    with SECURITY_IDENTIFICATION so the unelevated server cannot impersonate its admin token
#include "authorization_session.hpp"
#include "detail.hpp"
#include "rawdisk/block_device.hpp"
#include "remote.hpp"
#include "win32_common.hpp"
#include "win32_pipe.hpp"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

namespace rawdisk {
namespace {

using detail::fail;
using detail::Handle;
using detail::last_error;
using detail::win_error;

using detail::kPipePrefix;
using detail::kPipeRandomHex;

Result<std::filesystem::path> default_helper_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return fail(last_error());
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        if (buf.size() >= 32768)
            return fail(Errc::internal_error);
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path() / L"axklib_helper.exe";
}

struct ComInit {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComInit() {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
};

// Shows the UAC prompt. Returns the helper's process handle.
Result<Handle> launch_helper(const std::filesystem::path &exe, const std::wstring &pipe_name, void *owner) {
    const DWORD attrs = ::GetFileAttributesW(exe.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return fail(Errc::elevation_failed);

    const std::wstring params = std::format(L"--pipe {} --parent {}", pipe_name, ::GetCurrentProcessId());
    ComInit com; // recommended for ShellExecuteEx; harmless if COM is already initialized differently
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = static_cast<HWND>(owner);
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&sei)) {
        const DWORD e = ::GetLastError();
        return fail(e == ERROR_CANCELLED ? make_error_code(Errc::elevation_cancelled) : win_error(e));
    }
    if (!sei.hProcess)
        return fail(Errc::elevation_failed);
    return Handle{sei.hProcess};
}

struct Win32AuthorizationState final : detail::AuthorizationState {
    std::shared_ptr<detail::RemoteSession> session;
};

} // namespace

static Result<std::shared_ptr<detail::RemoteSession>> start_helper(const ElevationOptions &options) {
    std::filesystem::path exe = options.helper_path;
    if (exe.empty()) {
        auto d = default_helper_path();
        if (!d)
            return fail(d.error());
        exe = std::move(*d);
    }
    auto name = detail::random_pipe_name();
    if (!name)
        return fail(name.error());
    auto pipe = detail::create_pipe(*name);
    if (!pipe)
        return fail(pipe.error());
    auto process = launch_helper(exe, *name, options.parent_window);
    if (!process)
        return fail(process.error());
    // If this fails the helper cannot be terminated from here (it is elevated); it exits on
    // its own once the pipe is gone.
    if (auto r = detail::await_helper(pipe->h, process->h, options.connect_timeout_ms); !r)
        return fail(r.error());

    auto stream = detail::make_pipe_stream(std::move(*pipe), options.io_timeout_ms);
    if (!stream)
        return fail(stream.error());
    auto session = detail::RemoteSession::start(std::move(*stream));
    if (!session)
        return fail(session.error());
    return session;
}

Result<std::shared_ptr<detail::RemoteSession>> detail::win32_elevated_session(AuthorizationSession &authorization,
                                                                              const HelperSessionLauncher &launcher) {
    const auto &options = AuthorizationSessionAccess::options(authorization);
    if (!options.allow)
        return fail(std::make_error_code(std::errc::permission_denied));
    const std::lock_guard lock{AuthorizationSessionAccess::mutex(authorization)};
    auto &slot = AuthorizationSessionAccess::state(authorization);
    if (!slot)
        slot = std::make_shared<Win32AuthorizationState>();
    auto state = std::dynamic_pointer_cast<Win32AuthorizationState>(slot);
    if (!state)
        return fail(Errc::internal_error);
    if (state->session && !state->session->broken())
        return state->session;
    state->session.reset();
    auto started = launcher ? launcher(options) : start_helper(options);
    if (!started)
        return fail(started.error());
    if (!*started || (*started)->broken())
        return fail(Errc::elevation_failed);
    state->session = std::move(*started);
    return state->session;
}

int helper_main(int argc, wchar_t **argv) noexcept {
    try {
        std::wstring pipe_name;
        DWORD parent = 0;
        if (argc != 5)
            return 2;
        for (int i = 1; i + 1 < argc; i += 2) {
            const std::wstring_view key = argv[i];
            const std::wstring_view value = argv[i + 1];
            if (key == L"--pipe") {
                pipe_name = value;
            } else if (key == L"--parent") {
                if (value.empty() || value.size() > 10 ||
                    !std::ranges::all_of(value, [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                    return 2;
                parent = static_cast<DWORD>(std::wcstoul(std::wstring(value).c_str(), nullptr, 10));
            } else {
                return 2;
            }
        }
        const std::wstring_view suffix =
            std::wstring_view(pipe_name).substr(std::min(pipe_name.size(), kPipePrefix.size()));
        if (!pipe_name.starts_with(kPipePrefix) || suffix.size() != kPipeRandomHex ||
            !std::ranges::all_of(suffix,
                                 [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }) ||
            parent == 0)
            return 2;

        Handle pipe{::CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
        if (!pipe.valid())
            return 3;
        ULONG server = 0;
        if (!::GetNamedPipeServerProcessId(pipe.h, &server) || server != parent)
            return 4;

        ElevationOptions no_elevation;
        no_elevation.allow = false; // the helper must never try to elevate again
        auto authorization = create_authorization_session(std::move(no_elevation));
        if (!authorization)
            return 5;
        auto stream = detail::make_pipe_stream(std::move(pipe), INFINITE);
        if (!stream)
            return 5;
        const auto result = detail::serve(**stream, [&](const DeviceInfo &info, Access access) {
            return open_device(info, access, *authorization); // full enumeration + identity + capacity checks
        });
        return result ? 0 : 6;
    } catch (...) {
        return 7;
    }
}

} // namespace rawdisk
