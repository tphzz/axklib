#include "linux_authorization.hpp"
#include "detail.hpp"

#include <cerrno>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>

namespace rawdisk::detail {
namespace {

constexpr const char *service = "org.freedesktop.UDisks2";

class BusError {
  public:
    BusError() { dbus_error_init(&value); }
    ~BusError() { dbus_error_free(&value); }
    BusError(const BusError &) = delete;
    BusError &operator=(const BusError &) = delete;
    DBusError value{};
};

struct ConnectionDeleter {
    void operator()(DBusConnection *connection) const noexcept {
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
    }
};

bool dictionary(DBusMessageIter &parent, const char *key = nullptr, int type = DBUS_TYPE_INVALID,
                const void *value = nullptr) {
    DBusMessageIter array = DBUS_MESSAGE_ITER_INIT_CLOSED;
    DBusMessageIter entry = DBUS_MESSAGE_ITER_INIT_CLOSED;
    DBusMessageIter variant = DBUS_MESSAGE_ITER_INIT_CLOSED;
    bool ok = dbus_message_iter_open_container(&parent, DBUS_TYPE_ARRAY, "{sv}", &array);
    if (ok && key) {
        const char signature[]{static_cast<char>(type), '\0'};
        ok = dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) &&
             dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) &&
             dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant) &&
             dbus_message_iter_append_basic(&variant, type, value) &&
             dbus_message_iter_close_container(&entry, &variant) && dbus_message_iter_close_container(&array, &entry);
    }
    if (ok)
        ok = dbus_message_iter_close_container(&parent, &array);
    dbus_message_iter_abandon_container_if_open(&entry, &variant);
    dbus_message_iter_abandon_container_if_open(&array, &entry);
    dbus_message_iter_abandon_container_if_open(&parent, &array);
    return ok;
}

Result<std::string> resolved_path(DBusMessage *reply) {
    if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN || !dbus_message_has_signature(reply, "ao"))
        return fail(Errc::helper_protocol);
    DBusMessageIter root{}, paths{};
    dbus_message_iter_init(reply, &root);
    dbus_message_iter_recurse(&root, &paths);
    if (dbus_message_iter_get_arg_type(&paths) == DBUS_TYPE_INVALID)
        return fail(Errc::device_changed);
    const char *path = nullptr;
    dbus_message_iter_get_basic(&paths, &path);
    if (!path || !std::string_view(path).starts_with("/org/freedesktop/UDisks2/block_devices/") ||
        dbus_message_iter_next(&paths))
        return fail(Errc::helper_protocol);
    return std::string(path);
}

Result<PosixFd> authorized_descriptor(DBusMessage *reply, Access access) {
    if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN || !dbus_message_has_signature(reply, "h"))
        return fail(Errc::helper_protocol);
    DBusMessageIter iter{};
    dbus_message_iter_init(reply, &iter);
    int received = -1;
    // libdbus duplicates the descriptor; the message and caller each own their copy.
    dbus_message_iter_get_basic(&iter, &received);
    PosixFd fd(received);
    if (!fd.valid())
        return fail(Errc::elevation_failed);
    return validate_authorized_fd(std::move(fd), access);
}

} // namespace

std::error_code linux_authorization_error(std::string_view name) {
    if (name == "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed")
        return make_error_code(Errc::elevation_cancelled);
    if (name == "org.freedesktop.UDisks2.Error.NotAuthorized" ||
        name == "org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain" || name == DBUS_ERROR_ACCESS_DENIED ||
        name == DBUS_ERROR_AUTH_FAILED)
        return make_error_code(Errc::authorization_denied);
    if (name == DBUS_ERROR_NO_REPLY || name == DBUS_ERROR_TIMEOUT || name == DBUS_ERROR_TIMED_OUT)
        return make_error_code(Errc::authorization_timeout);
    if (name == DBUS_ERROR_SERVICE_UNKNOWN || name == DBUS_ERROR_NAME_HAS_NO_OWNER ||
        name == DBUS_ERROR_FILE_NOT_FOUND || name == DBUS_ERROR_UNKNOWN_METHOD || name == DBUS_ERROR_NOT_SUPPORTED ||
        name == DBUS_ERROR_DISCONNECTED)
        return make_error_code(Errc::authorization_unavailable);
    if (name == "org.freedesktop.UDisks2.Error.DeviceBusy")
        return make_error_code(Errc::device_busy);
    if (name == DBUS_ERROR_NO_MEMORY)
        return std::make_error_code(std::errc::not_enough_memory);
    return make_error_code(Errc::elevation_failed);
}

Result<PosixFd> linux_authorization_exchange(const std::string &path, Access access, AuthorizationDeadline deadline,
                                             const AuthorizationCall &call) {
    if (std::chrono::steady_clock::now() >= deadline)
        return fail(Errc::authorization_timeout);
    DbusMessage resolve(dbus_message_new_method_call(service, "/org/freedesktop/UDisks2/Manager",
                                                     "org.freedesktop.UDisks2.Manager", "ResolveDevice"));
    if (!resolve)
        return fail(std::make_error_code(std::errc::not_enough_memory));
    DBusMessageIter args{};
    dbus_message_iter_init_append(resolve.get(), &args);
    const char *device_path = path.c_str();
    if (!dictionary(args, "path", DBUS_TYPE_STRING, &device_path) || !dictionary(args))
        return fail(std::make_error_code(std::errc::not_enough_memory));
    auto resolved = call(resolve.get(), deadline);
    if (!resolved)
        return fail(resolved.error());
    auto object_path = resolved_path(resolved->get());
    if (!object_path)
        return fail(object_path.error());
    if (std::chrono::steady_clock::now() >= deadline)
        return fail(Errc::authorization_timeout);

    DbusMessage open(
        dbus_message_new_method_call(service, object_path->c_str(), "org.freedesktop.UDisks2.Block", "OpenDevice"));
    if (!open)
        return fail(std::make_error_code(std::errc::not_enough_memory));
    dbus_message_set_allow_interactive_authorization(open.get(), true);
    dbus_message_iter_init_append(open.get(), &args);
    const char *mode = access == Access::ReadWrite ? "rw" : "r";
    const dbus_int32_t flags = O_DIRECT | O_CLOEXEC | O_NOCTTY | (access == Access::ReadWrite ? O_EXCL : 0);
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &mode) ||
        !dictionary(args, "flags", DBUS_TYPE_INT32, &flags))
        return fail(std::make_error_code(std::errc::not_enough_memory));
    auto opened = call(open.get(), deadline);
    if (!opened)
        return fail(opened.error());
    if (std::chrono::steady_clock::now() >= deadline)
        return fail(Errc::authorization_timeout);
    return authorized_descriptor(opened->get(), access);
}

Result<PosixFd> linux_authorized_open(const std::string &path, Access access) {
    if (!elevation_options().allow)
        return fail(std::make_error_code(std::errc::permission_denied));
    static const bool initialized = dbus_threads_init_default();
    if (!initialized)
        return fail(std::make_error_code(std::errc::not_enough_memory));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    BusError error;
    std::unique_ptr<DBusConnection, ConnectionDeleter> connection(dbus_bus_get_private(DBUS_BUS_SYSTEM, &error.value));
    if (!connection)
        return fail(linux_authorization_error(error.value.name ? error.value.name : DBUS_ERROR_DISCONNECTED));
    dbus_connection_set_exit_on_disconnect(connection.get(), false);
    if (!dbus_connection_can_send_type(connection.get(), DBUS_TYPE_UNIX_FD))
        return fail(Errc::authorization_unavailable);
    return linux_authorization_exchange(
        path, access, deadline, [&](DBusMessage *request, AuthorizationDeadline until) -> Result<DbusMessage> {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return fail(Errc::authorization_timeout);
            BusError call_error;
            DbusMessage reply(dbus_connection_send_with_reply_and_block(
                connection.get(), request, static_cast<int>(remaining.count()), &call_error.value));
            if (!reply)
                return fail(
                    linux_authorization_error(call_error.value.name ? call_error.value.name : DBUS_ERROR_FAILED));
            return reply;
        });
}

} // namespace rawdisk::detail
