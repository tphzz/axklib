#include "authorization_session.hpp"
#include "posix_authorization.hpp"
#include "test_util.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <Security/Authorization.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

using namespace rawdisk;
using namespace rawdisk::detail;
using namespace std::chrono_literals;

namespace {

struct TestAuthorization {
    AuthorizationRef reference{};
    ~TestAuthorization() {
        if (reference)
            (void)::AuthorizationFree(reference, kAuthorizationFlagDefaults);
    }
};

bool test_native_authorization_preflight() {
    auto checked = [](const char *operation, OSStatus status) {
        if (status != errAuthorizationSuccess)
            std::fprintf(stderr, "%s failed: OSStatus=%ld\n", operation, static_cast<long>(status));
        CHECK(status == errAuthorizationSuccess);
        return status == errAuthorizationSuccess;
    };
    TestAuthorization original;
    if (!checked("AuthorizationCreate", ::AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment,
                                                              kAuthorizationFlagDefaults, &original.reference)))
        return false;
    AuthorizationExternalForm form{};
    if (!checked("AuthorizationMakeExternalForm", ::AuthorizationMakeExternalForm(original.reference, &form)))
        return false;
    TestAuthorization imported;
    return checked("AuthorizationCreateFromExternalForm",
                   ::AuthorizationCreateFromExternalForm(&form, &imported.reference));
}

int child_main(int argc, char **argv) {
    if (argc != 6 || std::string_view(argv[1]) != "-stdoutpipe" || std::string_view(argv[2]) != "-extauth" ||
        std::string_view(argv[3]) != "-o")
        return 3;
    constexpr std::string_view prefix = "/dev/auth-test-";
    const std::string path(argv[5]);
    if (!path.starts_with(prefix))
        return 3;
    const int foreign = std::stoi(path.substr(prefix.size()));
    if (::fcntl(foreign, F_GETFD) != -1 || errno != EBADF)
        return 3;
    AuthorizationExternalForm form{};
    auto *bytes = reinterpret_cast<char *>(&form);
    std::size_t count{};
    while (count < sizeof form) {
        const auto received = ::read(STDIN_FILENO, bytes + count, sizeof form - count);
        if (received <= 0)
            return 3;
        count += static_cast<std::size_t>(received);
    }
    AuthorizationRef imported{};
    if (::AuthorizationCreateFromExternalForm(&form, &imported) != errAuthorizationSuccess)
        return 3;
    (void)::AuthorizationFree(imported, kAuthorizationFlagDefaults);
    const int mode = std::stoi(argv[4]);
    if (mode != O_RDONLY && mode != O_RDWR)
        return 3;
    PosixFd descriptor(::open("/dev/null", mode | O_CLOEXEC));
    if (!descriptor.valid())
        return 3;
    char byte{};
    iovec iov{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    auto *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    const int fd = descriptor.get();
    std::memcpy(CMSG_DATA(header), &fd, sizeof fd);
    return ::sendmsg(STDOUT_FILENO, &message, 0) == 1 ? 0 : 3;
}

void test_reference_reuse_and_private_spawn(const std::string &executable) {
    auto created = create_authorization_session();
    CHECK(created.has_value());
    if (!created)
        return;
    auto session = std::move(*created);
    CHECK(!AuthorizationSessionAccess::state(*session));
    PosixFd ordinary(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    CHECK(ordinary.valid());
    PosixFd foreign(::fcntl(ordinary.get(), F_DUPFD, 64));
    CHECK(foreign.valid());
    if (!foreign.valid())
        return;
    const auto path = "/dev/auth-test-" + std::to_string(foreign.get());
    unsigned processes{};
    const AuthorizedOpenProcess process = [&](const std::string &requested, Access access,
                                              std::span<const std::byte> form, AuthorizationDeadline deadline) {
        CHECK(requested == path);
        CHECK(form.size() == sizeof(AuthorizationExternalForm));
        ++processes;
        return run_macos_authorization_process(requested, access, form, deadline, executable);
    };
    std::weak_ptr<AuthorizationState> original;
    for (const auto access : {Access::ReadOnly, Access::ReadOnly, Access::ReadWrite}) {
        auto opened = macos_authorized_open(path, access, *session, std::chrono::steady_clock::now() + 5s, process);
        if (!opened)
            std::fprintf(stderr, "macos_authorized_open failed: %s:%d (%s)\n", opened.error().category().name(),
                         opened.error().value(), opened.error().message().c_str());
        CHECK(opened.has_value());
        if (opened) {
            CHECK((::fcntl(opened->get(), F_GETFL) & O_ACCMODE) == (access == Access::ReadWrite ? O_RDWR : O_RDONLY));
            CHECK((::fcntl(opened->get(), F_GETFD) & FD_CLOEXEC) != 0);
        }
        if (original.expired())
            original = AuthorizationSessionAccess::state(*session);
        else
            CHECK(original.lock() == AuthorizationSessionAccess::state(*session));
    }
    CHECK(processes == 3U);
    const AuthorizedOpenProcess cancelled = [](const auto &, Access, auto, auto) -> Result<PosixFd> {
        return std::unexpected(make_error_code(Errc::elevation_cancelled));
    };
    CHECK(err(macos_authorized_open(path, Access::ReadOnly, *session, std::chrono::steady_clock::now() + 1s,
                                    cancelled)) == Errc::elevation_cancelled);
    CHECK(original.lock() == AuthorizationSessionAccess::state(*session));
    CHECK(macos_authorized_open(path, Access::ReadOnly, *session, std::chrono::steady_clock::now() + 5s, process)
              .has_value());
    auto independent = create_authorization_session();
    CHECK(independent.has_value());
    if (independent)
        CHECK(!AuthorizationSessionAccess::state(**independent));
    session.reset();
    CHECK(original.expired());
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1)
        return child_main(argc, argv);
    if (!test_native_authorization_preflight())
        return finish_tests();
    test_reference_reuse_and_private_spawn(std::filesystem::absolute(argv[0]).string());
    return finish_tests();
}
