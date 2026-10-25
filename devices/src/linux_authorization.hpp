#pragma once

#include "posix_authorization.hpp"
#include "rawdisk/block_device.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <dbus/dbus.h>

namespace rawdisk::detail {

struct DbusMessageDeleter {
    void operator()(DBusMessage *message) const noexcept { dbus_message_unref(message); }
};
using DbusMessage = std::unique_ptr<DBusMessage, DbusMessageDeleter>;
using AuthorizationCall = std::function<Result<DbusMessage>(DBusMessage *, AuthorizationDeadline)>;

class AuthorizationConnection {
  public:
    virtual ~AuthorizationConnection() = default;
    virtual Result<DbusMessage> call(DBusMessage *, AuthorizationDeadline) = 0;
};
using AuthorizationConnectionFactory = std::function<Result<std::unique_ptr<AuthorizationConnection>>()>;

std::error_code linux_authorization_error(std::string_view name);
Result<PosixFd> linux_authorization_exchange(const std::string &path, Access access, AuthorizationDeadline deadline,
                                             const AuthorizationCall &call);
Result<PosixFd> linux_authorized_open(const std::string &path, Access access, AuthorizationSession &authorization,
                                      AuthorizationDeadline deadline = std::chrono::steady_clock::now() +
                                                                       std::chrono::seconds(120),
                                      const AuthorizationConnectionFactory &factory = {});

} // namespace rawdisk::detail
