// Runs the production helper without requesting elevation. No physical sector I/O is requested.
#include "test_util.hpp"
#include "win32_pipe.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

#include <windows.h>

using namespace rawdisk;
using rawdisk::detail::Handle;

namespace {

constexpr DWORD kWaitMs = 5'000;

class ChildProcess {
  public:
    ~ChildProcess() {
        if (!process_.valid())
            return;
        // A failed assertion must not leave a helper waiting for its client forever.
        if (::WaitForSingleObject(process_.h, 1000) != WAIT_OBJECT_0) {
            CHECK(::TerminateJobObject(job_.h, 99) != FALSE);
            CHECK(::WaitForSingleObject(process_.h, kWaitMs) == WAIT_OBJECT_0);
            CHECK(false);
        }
    }

    bool start(const std::filesystem::path &executable, const std::wstring &arguments) {
        job_ = Handle{::CreateJobObjectW(nullptr, nullptr)};
        CHECK(job_.valid());
        if (!job_.valid())
            return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const BOOL configured = ::SetInformationJobObject(job_.h, JobObjectExtendedLimitInformation, &limits,
                                                          static_cast<DWORD>(sizeof limits));
        CHECK(configured != FALSE);
        if (!configured)
            return false;

        std::wstring command = std::format(L"\"{}\" {}", executable.native(), arguments);
        STARTUPINFOW startup{};
        startup.cb = static_cast<DWORD>(sizeof startup);
        PROCESS_INFORMATION created{};
        const BOOL launched =
            ::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &created);
        CHECK(launched != FALSE);
        if (!launched)
            return false;
        process_ = Handle{created.hProcess};
        Handle thread{created.hThread};
        const BOOL assigned = ::AssignProcessToJobObject(job_.h, process_.h);
        CHECK(assigned != FALSE);
        if (!assigned) {
            CHECK(::TerminateProcess(process_.h, 99) != FALSE);
            CHECK(::WaitForSingleObject(process_.h, kWaitMs) == WAIT_OBJECT_0);
            return false;
        }
        const DWORD resumed = ::ResumeThread(thread.h);
        CHECK(resumed != static_cast<DWORD>(-1));
        return resumed != static_cast<DWORD>(-1);
    }

    HANDLE handle() const noexcept { return process_.h; }

    void expect_exit(DWORD expected) {
        const DWORD waited = ::WaitForSingleObject(process_.h, kWaitMs);
        CHECK(waited == WAIT_OBJECT_0);
        if (waited != WAIT_OBJECT_0)
            return;
        DWORD code = STILL_ACTIVE;
        CHECK(::GetExitCodeProcess(process_.h, &code) != FALSE);
        CHECK(code == expected);
    }

  private:
    Handle job_;
    Handle process_;
};

bool launch_connected(const std::filesystem::path &helper, ChildProcess &child, Handle &pipe) {
    auto name = detail::random_pipe_name();
    CHECK(name.has_value());
    if (!name)
        return false;
    auto created = detail::create_pipe(*name);
    CHECK(created.has_value());
    if (!created)
        return false;
    pipe = std::move(*created);
    if (!child.start(helper, std::format(L"--pipe {} --parent {}", *name, ::GetCurrentProcessId())))
        return false;
    const auto connected = detail::await_helper(pipe.h, child.handle(), kWaitMs);
    CHECK(connected.has_value());
    return connected.has_value();
}

void test_invalid_arguments(const std::filesystem::path &helper) {
    for (const std::wstring &args :
         {std::wstring{}, std::wstring{L"--pipe bad --parent 1"}, std::wstring{L"--pipe bad --parent -1"}}) {
        ChildProcess child;
        if (child.start(helper, args))
            child.expect_exit(2);
    }
}

void test_first_instance_and_connect_timeout() {
    auto name = detail::random_pipe_name();
    CHECK(name.has_value());
    if (!name)
        return;
    auto pipe = detail::create_pipe(*name);
    CHECK(pipe.has_value());
    if (!pipe)
        return;
    CHECK(!detail::create_pipe(*name));
    CHECK(err(detail::await_helper(pipe->h, ::GetCurrentProcess(), 50)) == Errc::elevation_failed);
}

void test_session_and_rejected_device(const std::filesystem::path &helper) {
    ChildProcess child;
    Handle pipe;
    if (!launch_connected(helper, child, pipe))
        return;
    {
        auto stream = detail::make_pipe_stream(std::move(pipe), kWaitMs);
        CHECK(stream.has_value());
        if (!stream)
            return;
        auto session = detail::RemoteSession::start(std::move(*stream));
        CHECK(session.has_value());
        if (!session)
            return;
        // This cannot match a USB device; the production opener must reject it.
        DeviceInfo invalid;
        invalid.path = (test_dir() / "not-a-physical-device.img").string();
        invalid.sector_size = 512;
        invalid.size_bytes = 512;
        CHECK(err((*session)->open(invalid, Access::ReadOnly)) == Errc::device_changed);
        CHECK(!(*session)->broken());
    }
    child.expect_exit(0);
}

void test_wrong_parent(const std::filesystem::path &helper) {
    auto name = detail::random_pipe_name();
    CHECK(name.has_value());
    if (!name)
        return;
    auto pipe = detail::create_pipe(*name);
    CHECK(pipe.has_value());
    if (!pipe)
        return;
    ChildProcess child;
    const DWORD wrong_parent = ::GetCurrentProcessId() == 1 ? 2 : 1;
    if (child.start(helper, std::format(L"--pipe {} --parent {}", *name, wrong_parent)))
        child.expect_exit(4);
}

void test_wrong_client(const std::filesystem::path &helper) {
    ChildProcess child;
    Handle pipe;
    if (!launch_connected(helper, child, pipe))
        return;
    const HANDLE connected_pipe = pipe.h;
    {
        auto stream = detail::make_pipe_stream(std::move(pipe), kWaitMs);
        CHECK(stream.has_value());
        if (!stream)
            return;
        auto session = detail::RemoteSession::start(std::move(*stream));
        CHECK(session.has_value());
        if (!session)
            return;
        // Finish authentication first, so disconnect cannot race the helper's parent check.
        CHECK(err(detail::await_helper(connected_pipe, ::GetCurrentProcess(), kWaitMs)) == Errc::elevation_failed);
    }
    child.expect_exit(0);
}

void test_malformed_request(const std::filesystem::path &helper) {
    ChildProcess child;
    Handle pipe;
    if (!launch_connected(helper, child, pipe))
        return;
    {
        auto stream = detail::make_pipe_stream(std::move(pipe), kWaitMs);
        CHECK(stream.has_value());
        if (!stream)
            return;
        auto bytes = detail::encode_header({static_cast<std::uint16_t>(detail::Op::hello), 0, 0});
        bytes[0] = std::byte{0};
        CHECK((*stream)->write_all(bytes).has_value());
        child.expect_exit(6);
        std::array<std::byte, 1> response{};
        CHECK(!(*stream)->read_exact(response));
    }
}

void test_io_timeout_and_disconnect(const std::filesystem::path &helper) {
    ChildProcess child;
    Handle pipe;
    if (!launch_connected(helper, child, pipe))
        return;
    {
        auto stream = detail::make_pipe_stream(std::move(pipe), 50);
        CHECK(stream.has_value());
        if (!stream)
            return;
        // The helper waits for Hello; reading without sending it must time out safely.
        std::array<std::byte, 1> response{};
        CHECK(err((*stream)->read_exact(response)) == Errc::helper_timeout);
    }
    child.expect_exit(0);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path helper = argv[1];
    test_invalid_arguments(helper);
    test_first_instance_and_connect_timeout();
    test_session_and_rejected_device(helper);
    test_wrong_parent(helper);
    test_wrong_client(helper);
    test_malformed_request(helper);
    test_io_timeout_and_disconnect(helper);
    return finish_tests();
}
