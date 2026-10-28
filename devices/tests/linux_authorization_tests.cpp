#include "linux_authorization.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstring>
#include <expected>
#include <string_view>
#include <system_error>

#include <fcntl.h>

using namespace rawdisk;
using namespace rawdisk::detail;

namespace {

DbusMessage paths(bool multiple = false) {
    DbusMessage reply(dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN));
    DBusMessageIter args{}, array{};
    dbus_message_iter_init_append(reply.get(), &args);
    CHECK(dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "o", &array));
    const char *path = "/org/freedesktop/UDisks2/block_devices/test";
    CHECK(dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, &path));
    if (multiple)
        CHECK(dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, &path));
    CHECK(dbus_message_iter_close_container(&args, &array));
    return reply;
}

DbusMessage descriptor(int flags) {
    PosixFd fd(::open("/dev/null", flags | O_CLOEXEC));
    CHECK(fd.valid());
    DbusMessage reply(dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN));
    const int raw = fd.get();
    CHECK(dbus_message_append_args(reply.get(), DBUS_TYPE_UNIX_FD, &raw, DBUS_TYPE_INVALID));
    return reply;
}

void test_exchange(Access access) {
    unsigned calls{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto opened = linux_authorization_exchange(
        "/dev/test", access, deadline, [&](DBusMessage *request, AuthorizationDeadline until) -> Result<DbusMessage> {
            CHECK(until == deadline);
            CHECK(std::string_view(dbus_message_get_destination(request)) == "org.freedesktop.UDisks2");
            if (++calls == 1U) {
                CHECK(std::string_view(dbus_message_get_member(request)) == "ResolveDevice");
                CHECK(dbus_message_has_signature(request, "a{sv}a{sv}"));
                DBusMessageIter args{}, array{}, entry{}, value{};
                dbus_message_iter_init(request, &args);
                dbus_message_iter_recurse(&args, &array);
                dbus_message_iter_recurse(&array, &entry);
                const char *key = nullptr;
                dbus_message_iter_get_basic(&entry, &key);
                CHECK(std::string_view(key) == "path");
                dbus_message_iter_next(&entry);
                dbus_message_iter_recurse(&entry, &value);
                const char *path = nullptr;
                dbus_message_iter_get_basic(&value, &path);
                CHECK(std::string_view(path) == "/dev/test");
                return paths();
            }
            CHECK(std::string_view(dbus_message_get_member(request)) == "OpenDevice");
            CHECK(dbus_message_has_signature(request, "sa{sv}"));
            CHECK(dbus_message_get_allow_interactive_authorization(request));
            DBusMessageIter args{}, array{}, entry{}, value{};
            dbus_message_iter_init(request, &args);
            const char *mode = nullptr;
            dbus_message_iter_get_basic(&args, &mode);
            CHECK(std::string_view(mode) == (access != Access::ReadOnly ? "rw" : "r"));
            dbus_message_iter_next(&args);
            dbus_message_iter_recurse(&args, &array);
            dbus_message_iter_recurse(&array, &entry);
            const char *key = nullptr;
            dbus_message_iter_get_basic(&entry, &key);
            CHECK(std::string_view(key) == "flags");
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &value);
            dbus_int32_t flags{};
            dbus_message_iter_get_basic(&value, &flags);
            CHECK((flags & O_ACCMODE) == 0);
            CHECK((flags & O_DIRECT) != 0);
            CHECK((flags & O_CLOEXEC) != 0);
            CHECK(((flags & O_EXCL) != 0) == (access != Access::ReadOnly));
            return descriptor(access != Access::ReadOnly ? O_RDWR : O_RDONLY);
        });
    CHECK(opened.has_value());
    CHECK(calls == 2U);
    if (opened)
        CHECK((::fcntl(opened->get(), F_GETFD) & FD_CLOEXEC) != 0);
}

void test_failures() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto wrong_access = linux_authorization_exchange(
        "/dev/test", Access::ReadOnly, deadline,
        [](DBusMessage *request, AuthorizationDeadline) -> Result<DbusMessage> {
            return std::strcmp(dbus_message_get_member(request), "ResolveDevice") == 0 ? paths() : descriptor(O_RDWR);
        });
    CHECK(err(wrong_access) == Errc::helper_protocol);
    auto ambiguous = linux_authorization_exchange(
        "/dev/test", Access::ReadOnly, deadline,
        [](DBusMessage *, AuthorizationDeadline) -> Result<DbusMessage> { return paths(true); });
    CHECK(err(ambiguous) == Errc::helper_protocol);
    auto malformed = linux_authorization_exchange(
        "/dev/test", Access::ReadOnly, deadline, [](DBusMessage *, AuthorizationDeadline) -> Result<DbusMessage> {
            return DbusMessage(dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN));
        });
    CHECK(err(malformed) == Errc::helper_protocol);
    auto timed_out = linux_authorization_exchange("/dev/test", Access::ReadOnly, std::chrono::steady_clock::now(),
                                                  [](DBusMessage *, AuthorizationDeadline) -> Result<DbusMessage> {
                                                      CHECK(false);
                                                      return paths();
                                                  });
    CHECK(err(timed_out) == Errc::authorization_timeout);
    auto cancelled = linux_authorization_exchange(
        "/dev/test", Access::ReadOnly, deadline, [](DBusMessage *, AuthorizationDeadline) -> Result<DbusMessage> {
            return std::unexpected(make_error_code(Errc::elevation_cancelled));
        });
    CHECK(err(cancelled) == Errc::elevation_cancelled);
    ElevationOptions options;
    options.allow = false;
    auto authorization = create_authorization_session(options);
    CHECK(authorization.has_value());
    if (authorization)
        CHECK(err(linux_authorized_open("/dev/test", Access::ReadOnly, **authorization)) ==
              std::errc::permission_denied);
    CHECK(linux_authorization_error("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed") ==
          Errc::elevation_cancelled);
    CHECK(linux_authorization_error("org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain") ==
          Errc::authorization_denied);
    CHECK(linux_authorization_error(DBUS_ERROR_SERVICE_UNKNOWN) == Errc::authorization_unavailable);
    CHECK(linux_authorization_error(DBUS_ERROR_NO_REPLY) == Errc::authorization_timeout);
    CHECK(linux_authorization_error(DBUS_ERROR_UNKNOWN_METHOD) == Errc::authorization_unavailable);
    CHECK(linux_authorization_error("org.freedesktop.UDisks2.Error.NotAuthorized") == Errc::authorization_denied);
    CHECK(linux_authorization_error("org.freedesktop.UDisks2.Error.DeviceBusy") == Errc::device_busy);
}

} // namespace

int main() {
    test_exchange(Access::ReadOnly);
    test_exchange(Access::ReadWrite);
    test_exchange(Access::ReadWriteUnmounted);
    test_failures();
    return finish_tests();
}
