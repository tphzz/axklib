#include "authorization_session.hpp"
#include "detail.hpp"
#include "posix_authorization.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <Security/Authorization.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

namespace rawdisk::detail {
namespace {

struct MacosAuthorizationState final : AuthorizationState {
    AuthorizationRef reference{};
    ~MacosAuthorizationState() override {
        if (reference)
            (void)::AuthorizationFree(reference, kAuthorizationFlagDefaults);
    }
};

struct ExternalAuthorization {
    AuthorizationExternalForm form{};
    ~ExternalAuthorization() {
        volatile unsigned char *bytes = reinterpret_cast<volatile unsigned char *>(&form);
        for (std::size_t index = 0; index < sizeof form; ++index)
            bytes[index] = 0;
    }
};

std::error_code authorization_error(OSStatus status) {
    if (status == errAuthorizationCanceled)
        return make_error_code(Errc::elevation_cancelled);
    if (status == errAuthorizationDenied)
        return make_error_code(Errc::authorization_denied);
    return make_error_code(Errc::elevation_failed);
}

class SpawnSetup {
  public:
    ~SpawnSetup() {
        if (actions_ready_)
            ::posix_spawn_file_actions_destroy(&actions);
        if (attributes_ready_)
            ::posix_spawnattr_destroy(&attributes);
    }
    Result<void> initialize(int ours, int theirs) {
        int result = ::posix_spawn_file_actions_init(&actions);
        if (result != 0)
            return fail(errno_code(result));
        actions_ready_ = true;
        result = ::posix_spawnattr_init(&attributes);
        if (result != 0)
            return fail(errno_code(result));
        attributes_ready_ = true;
        result = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
        if (result == 0)
            result = ::posix_spawn_file_actions_adddup2(&actions, theirs, STDIN_FILENO);
        if (result == 0)
            result = ::posix_spawn_file_actions_adddup2(&actions, theirs, STDOUT_FILENO);
        if (result == 0)
            result = ::posix_spawn_file_actions_addclose(&actions, ours);
        if (result == 0)
            result = ::posix_spawn_file_actions_addclose(&actions, theirs);
        if (result != 0)
            return fail(errno_code(result));
        return {};
    }
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};

  private:
    bool actions_ready_{};
    bool attributes_ready_{};
};

} // namespace

Result<PosixFd> run_macos_authorization_process(const std::string &path, Access access, std::span<const std::byte> form,
                                                AuthorizationDeadline deadline, const std::string &executable) {
    if (std::chrono::steady_clock::now() >= deadline)
        return fail(Errc::authorization_timeout);
    auto sockets = authorization_sockets();
    if (!sockets)
        return fail(sockets.error());
    auto &[ours, theirs] = *sockets;
    SpawnSetup setup;
    if (auto ready = setup.initialize(ours.get(), theirs.get()); !ready)
        return fail(ready.error());
    const std::string flags = std::to_string(access != Access::ReadOnly ? O_RDWR : O_RDONLY);
    const char *argv[]{executable.c_str(), "-stdoutpipe", "-extauth", "-o", flags.c_str(), path.c_str(), nullptr};
    pid_t pid{};
    const auto status =
        ::posix_spawn(&pid, argv[0], &setup.actions, &setup.attributes, const_cast<char *const *>(argv), environ);
    AuthorizedChild child(status == 0 ? pid : -1);
    if (status != 0)
        return fail(errno_code(status));
    {
        PosixFd close_now(std::move(theirs));
    }
    if (auto sent = send_authorization_bytes(ours.get(), form, deadline); !sent)
        return fail(sent.error());
    if (::shutdown(ours.get(), SHUT_WR) != 0)
        return fail(errno_code(errno));
    return receive_authorized_fd(ours.get(), child, access, deadline);
}

Result<PosixFd> macos_authorized_open(const std::string &path, Access access, AuthorizationSession &authorization,
                                      AuthorizationDeadline deadline, const AuthorizedOpenProcess &process) {
    if (!AuthorizationSessionAccess::options(authorization).allow)
        return fail(std::make_error_code(std::errc::permission_denied));
    auto lock = lock_authorization(authorization, deadline);
    if (!lock)
        return fail(lock.error());
    auto &slot = AuthorizationSessionAccess::state(authorization);
    if (!slot)
        slot = std::make_shared<MacosAuthorizationState>();
    auto state = std::dynamic_pointer_cast<MacosAuthorizationState>(slot);
    if (!state)
        return fail(Errc::internal_error);
    if (!state->reference) {
        const auto status = ::AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults,
                                                  &state->reference);
        if (status != errAuthorizationSuccess)
            return fail(authorization_error(status));
    }
    ExternalAuthorization external;
    const auto status = ::AuthorizationMakeExternalForm(state->reference, &external.form);
    if (status != errAuthorizationSuccess) {
        slot.reset();
        return fail(authorization_error(status));
    }
    const auto bytes = std::as_bytes(std::span{&external.form, std::size_t{1}});
    auto result = process ? process(path, access, bytes, deadline)
                          : run_macos_authorization_process(path, access, bytes, deadline, "/usr/libexec/authopen");
    if (!result)
        return result;
    return validate_authorized_fd(std::move(*result), access);
}

} // namespace rawdisk::detail
