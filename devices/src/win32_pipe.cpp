#include "win32_pipe.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <bcrypt.h>
#include <sddl.h>
#include <windows.h>

namespace rawdisk::detail {
namespace {

// The OVERLAPPED record must outlive cancellation of its pending operation.
class PipeStream final : public Stream {
  public:
    PipeStream(Handle pipe, DWORD timeout_ms)
        : pipe_(std::move(pipe)), event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)), timeout_(timeout_ms) {}

    bool valid() const noexcept { return pipe_.valid() && event_.valid(); }

    Result<void> read_exact(std::span<std::byte> out) override {
        while (!out.empty()) {
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size(), 1u << 20));
            OVERLAPPED ov = prepare();
            const BOOL ok = ::ReadFile(pipe_.h, out.data(), want, nullptr, &ov);
            DWORD n = 0;
            if (auto r = finish(ok, ov, n); !r)
                return r;
            if (n == 0)
                return fail(win_error(ERROR_BROKEN_PIPE));
            out = out.subspan(n);
        }
        return {};
    }

    Result<void> write_all(std::span<const std::byte> in) override {
        while (!in.empty()) {
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(in.size(), 1u << 20));
            OVERLAPPED ov = prepare();
            const BOOL ok = ::WriteFile(pipe_.h, in.data(), want, nullptr, &ov);
            DWORD n = 0;
            if (auto r = finish(ok, ov, n); !r)
                return r;
            if (n == 0)
                return fail(win_error(ERROR_BROKEN_PIPE));
            in = in.subspan(n);
        }
        return {};
    }

  private:
    OVERLAPPED prepare() noexcept {
        ::ResetEvent(event_.h);
        OVERLAPPED ov{};
        ov.hEvent = event_.h;
        return ov;
    }

    Result<void> finish(BOOL started_ok, OVERLAPPED &ov, DWORD &done) {
        if (!started_ok) {
            const DWORD e = ::GetLastError();
            if (e != ERROR_IO_PENDING)
                return fail(win_error(e));
        }
        if (::WaitForSingleObject(event_.h, timeout_) != WAIT_OBJECT_0) {
            ::CancelIoEx(pipe_.h, &ov);
            DWORD ignored = 0;
            ::GetOverlappedResult(pipe_.h, &ov, &ignored, TRUE);
            return fail(Errc::helper_timeout);
        }
        if (!::GetOverlappedResult(pipe_.h, &ov, &done, FALSE))
            return fail(last_error());
        return {};
    }

    Handle pipe_;
    Handle event_;
    DWORD timeout_;
};

Result<std::wstring> user_sid_string() {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok))
        return fail(last_error());
    Handle token{tok};
    DWORD len = 0;
    ::GetTokenInformation(token.h, TokenUser, nullptr, 0, &len);
    if (len == 0)
        return fail(last_error());
    std::vector<std::byte> buf(len);
    if (!::GetTokenInformation(token.h, TokenUser, buf.data(), len, &len))
        return fail(last_error());
    const auto *user = reinterpret_cast<const TOKEN_USER *>(buf.data());
    LPWSTR sid = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &sid))
        return fail(last_error());
    std::wstring out(sid);
    ::LocalFree(sid);
    return out;
}

} // namespace

Result<std::unique_ptr<Stream>> make_pipe_stream(Handle pipe, DWORD timeout_ms) {
    auto stream = std::make_unique<PipeStream>(std::move(pipe), timeout_ms);
    if (!stream->valid())
        return fail(Errc::internal_error);
    return std::unique_ptr<Stream>{std::move(stream)};
}

Result<std::wstring> random_pipe_name() {
    std::array<unsigned char, kPipeRandomHex / 2> rnd{};
    if (!BCRYPT_SUCCESS(
            ::BCryptGenRandom(nullptr, rnd.data(), static_cast<ULONG>(rnd.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return fail(Errc::internal_error);
    std::wstring name(kPipePrefix);
    for (unsigned char b : rnd)
        name += std::format(L"{:02x}", static_cast<unsigned>(b));
    return name;
}

Result<Handle> create_pipe(const std::wstring &name) {
    auto sid = user_sid_string();
    if (!sid)
        return fail(sid.error());
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + *sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return fail(last_error());
    SECURITY_ATTRIBUTES sa{static_cast<DWORD>(sizeof sa), sd, FALSE};
    Handle pipe{::CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1u << 20, 1u << 20, 0, &sa)};
    const DWORD e = ::GetLastError();
    ::LocalFree(sd);
    if (!pipe.valid())
        return fail(win_error(e));
    return pipe;
}

Result<void> await_helper(HANDLE pipe, HANDLE process, DWORD timeout_ms) {
    Handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid())
        return fail(last_error());
    OVERLAPPED ov{};
    ov.hEvent = event.h;

    if (!::ConnectNamedPipe(pipe, &ov)) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_IO_PENDING) {
            const HANDLE waits[2] = {event.h, process};
            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
            if (w != WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &ov);
                DWORD ignored = 0;
                ::GetOverlappedResult(pipe, &ov, &ignored, TRUE);
                return fail(Errc::elevation_failed);
            }
            DWORD ignored = 0;
            if (!::GetOverlappedResult(pipe, &ov, &ignored, FALSE))
                return fail(last_error());
        } else if (e != ERROR_PIPE_CONNECTED) {
            return fail(win_error(e));
        }
    }

    ULONG client = 0;
    if (!::GetNamedPipeClientProcessId(pipe, &client) || client != ::GetProcessId(process)) {
        ::DisconnectNamedPipe(pipe);
        return fail(Errc::elevation_failed);
    }
    return {};
}

} // namespace rawdisk::detail
