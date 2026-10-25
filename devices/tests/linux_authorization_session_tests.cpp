#include "authorization_session.hpp"
#include "linux_authorization.hpp"
#include "test_util.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dbus/dbus.h>
#include <fcntl.h>
#include <sys/file.h>

using namespace rawdisk;
using namespace rawdisk::detail;
using namespace std::chrono_literals;

namespace {

enum class Stage { resolve, open };
enum class Failure { timeout, cancelled, denied, protocol, wrong_access };

struct FailurePlan {
    Failure failure;
    Stage stage;
    bool queue_late_reply = false;
};

struct OpenRequest {
    std::string mode;
    dbus_int32_t flags;
    std::thread::id thread;
};

struct Trace {
    std::filesystem::path backing;
    unsigned connections{};
    unsigned destroyed{};
    unsigned resolve_calls{};
    unsigned open_calls{};
    std::vector<std::string> resolved_paths;
    std::vector<OpenRequest> opens;
    std::optional<FailurePlan> failure;
    bool queued_lock_observed{};
};

AuthorizationDeadline deadline() { return std::chrono::steady_clock::now() + 2s; }

DbusMessage paths(const std::string &object_path) {
    DbusMessage reply(dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN));
    DBusMessageIter root{}, array{};
    dbus_message_iter_init_append(reply.get(), &root);
    CHECK(dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "o", &array));
    const char *path = object_path.c_str();
    CHECK(dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, &path));
    CHECK(dbus_message_iter_close_container(&root, &array));
    return reply;
}

DbusMessage descriptor(int fd) {
    DbusMessage reply(dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN));
    CHECK(dbus_message_append_args(reply.get(), DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID));
    return reply;
}

DbusMessage descriptor(const std::filesystem::path &path, Access access) {
    PosixFd fd(::open(path.c_str(), O_CLOEXEC | (access == Access::ReadWrite ? O_RDWR : O_RDONLY)));
    CHECK(fd.valid());
    return descriptor(fd.get());
}

std::error_code failure_error(Failure failure) {
    switch (failure) {
    case Failure::timeout:
        return make_error_code(Errc::authorization_timeout);
    case Failure::cancelled:
        return make_error_code(Errc::elevation_cancelled);
    case Failure::denied:
        return make_error_code(Errc::authorization_denied);
    case Failure::protocol:
    case Failure::wrong_access:
        return make_error_code(Errc::helper_protocol);
    }
    return make_error_code(Errc::internal_error);
}

class FakeConnection final : public AuthorizationConnection {
  public:
    explicit FakeConnection(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) { ++trace_->connections; }
    ~FakeConnection() override { ++trace_->destroyed; }

    Result<DbusMessage> call(DBusMessage *request, AuthorizationDeadline until) override {
        CHECK(until > std::chrono::steady_clock::now());
        const bool resolving = std::string_view(dbus_message_get_member(request)) == "ResolveDevice";
        const Stage stage = resolving ? Stage::resolve : Stage::open;
        Access access = Access::ReadOnly;
        if (resolving) {
            ++trace_->resolve_calls;
            CHECK(dbus_message_has_signature(request, "a{sv}a{sv}"));
            if (!dbus_message_has_signature(request, "a{sv}a{sv}"))
                return std::unexpected(make_error_code(Errc::helper_protocol));
            DBusMessageIter root{}, array{}, entry{}, value{};
            dbus_message_iter_init(request, &root);
            dbus_message_iter_recurse(&root, &array);
            dbus_message_iter_recurse(&array, &entry);
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &value);
            const char *path = nullptr;
            dbus_message_iter_get_basic(&value, &path);
            trace_->resolved_paths.emplace_back(path);
            object_path_ = "/org/freedesktop/UDisks2/block_devices/session" + std::to_string(trace_->resolve_calls);
        } else {
            ++trace_->open_calls;
            CHECK(std::string_view(dbus_message_get_member(request)) == "OpenDevice");
            CHECK(std::string_view(dbus_message_get_path(request)) == object_path_);
            CHECK(dbus_message_has_signature(request, "sa{sv}"));
            if (!dbus_message_has_signature(request, "sa{sv}"))
                return std::unexpected(make_error_code(Errc::helper_protocol));
            CHECK(dbus_message_get_allow_interactive_authorization(request));
            DBusMessageIter root{}, array{}, entry{}, value{};
            dbus_message_iter_init(request, &root);
            const char *mode = nullptr;
            dbus_message_iter_get_basic(&root, &mode);
            access = std::string_view(mode) == "rw" ? Access::ReadWrite : Access::ReadOnly;
            dbus_message_iter_next(&root);
            dbus_message_iter_recurse(&root, &array);
            dbus_message_iter_recurse(&array, &entry);
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &value);
            dbus_int32_t flags{};
            dbus_message_iter_get_basic(&value, &flags);
            trace_->opens.push_back({mode, flags, std::this_thread::get_id()});
        }

        if (trace_->failure && trace_->failure->stage == stage) {
            const auto plan = *trace_->failure;
            trace_->failure.reset();
            if (plan.queue_late_reply)
                queue_late_reply();
            if (plan.failure == Failure::protocol)
                return DbusMessage(dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN));
            if (plan.failure == Failure::wrong_access)
                return descriptor(trace_->backing, access == Access::ReadWrite ? Access::ReadOnly : Access::ReadWrite);
            return std::unexpected(failure_error(plan.failure));
        }
        return resolving ? paths(object_path_) : descriptor(trace_->backing, access);
    }

  private:
    void queue_late_reply() {
        {
            PosixFd locked(::open(trace_->backing.c_str(), O_RDWR | O_CLOEXEC));
            CHECK(locked.valid());
            CHECK(::flock(locked.get(), LOCK_EX | LOCK_NB) == 0);
            queued_reply_ = descriptor(locked.get());
        }
        PosixFd contender(::open(trace_->backing.c_str(), O_RDWR | O_CLOEXEC));
        CHECK(contender.valid());
        errno = 0;
        CHECK(::flock(contender.get(), LOCK_EX | LOCK_NB) == -1);
        CHECK(errno == EWOULDBLOCK || errno == EAGAIN);
        trace_->queued_lock_observed = true;
    }

    std::shared_ptr<Trace> trace_;
    std::string object_path_;
    DbusMessage queued_reply_;
};

AuthorizationConnectionFactory factory_for(const std::shared_ptr<Trace> &trace) {
    return [trace]() -> Result<std::unique_ptr<AuthorizationConnection>> {
        return std::unique_ptr<AuthorizationConnection>(std::make_unique<FakeConnection>(trace));
    };
}

void check_closed(int fd) {
    errno = 0;
    CHECK(::fcntl(fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
}

void open_and_close(AuthorizationSession &session, const AuthorizationConnectionFactory &factory,
                    const std::string &path, Access access = Access::ReadOnly) {
    int fd = -1;
    {
        auto opened = linux_authorized_open(path, access, session, deadline(), factory);
        CHECK(opened.has_value());
        if (!opened)
            return;
        fd = opened->get();
        CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((::fcntl(fd, F_GETFL) & O_ACCMODE) == (access == Access::ReadWrite ? O_RDWR : O_RDONLY));
    }
    check_closed(fd);
}

void test_connection_reused_across_workers_and_empty_device_gap() {
    auto session = create_authorization_session();
    CHECK(session.has_value());
    if (!session)
        return;
    auto trace = std::make_shared<Trace>();
    trace->backing = make_card("session-reuse.img", 2);
    const auto factory = factory_for(trace);
    CHECK(!AuthorizationSessionAccess::state(**session));
    open_and_close(**session, factory, "/dev/session-a");
    CHECK(trace->connections == 1U);
    CHECK(trace->destroyed == 0U);

    std::promise<void> second_started;
    auto started = second_started.get_future();
    std::promise<void> release_second;
    auto released = release_second.get_future();
    auto first_worker = std::async(std::launch::async, [&]() -> Result<PosixFd> {
        auto opened = linux_authorized_open("/dev/session-b", Access::ReadOnly, **session, deadline(), factory);
        if (started.wait_for(2s) != std::future_status::ready)
            return std::unexpected(make_error_code(Errc::authorization_timeout));
        return opened;
    });
    auto second_worker = std::async(std::launch::async, [&]() -> Result<PosixFd> {
        second_started.set_value();
        if (released.wait_for(5s) != std::future_status::ready)
            return std::unexpected(make_error_code(Errc::authorization_timeout));
        return linux_authorized_open("/dev/session-c", Access::ReadOnly, **session, deadline(), factory);
    });
    int first_fd = -1;
    {
        auto opened = first_worker.get();
        CHECK(opened.has_value());
        if (opened)
            first_fd = opened->get();
    }
    if (first_fd >= 0)
        check_closed(first_fd);
    CHECK(trace->connections == 1U);
    CHECK(trace->destroyed == 0U);
    release_second.set_value();
    {
        auto opened = second_worker.get();
        CHECK(opened.has_value());
    }
    open_and_close(**session, factory, "/dev/session-d", Access::ReadWrite);

    CHECK(trace->connections == 1U);
    CHECK(trace->destroyed == 0U);
    CHECK(trace->resolve_calls == 4U);
    CHECK(trace->open_calls == 4U);
    CHECK(trace->resolved_paths ==
          std::vector<std::string>({"/dev/session-a", "/dev/session-b", "/dev/session-c", "/dev/session-d"}));
    CHECK(trace->opens.size() == 4U);
    if (trace->opens.size() == 4U) {
        CHECK(trace->opens[1].thread != trace->opens[2].thread);
        CHECK(trace->opens[1].thread != std::this_thread::get_id());
        CHECK(trace->opens[2].thread != std::this_thread::get_id());
        for (std::size_t i = 0; i < trace->opens.size(); ++i) {
            const bool writable = i == 3U;
            CHECK(trace->opens[i].mode == (writable ? "rw" : "r"));
            CHECK(trace->opens[i].flags == (O_DIRECT | O_CLOEXEC | O_NOCTTY | (writable ? O_EXCL : 0)));
        }
    }
    session->reset();
    CHECK(trace->destroyed == 1U);
}

void test_contexts_do_not_share_connections() {
    auto first = create_authorization_session();
    auto second = create_authorization_session();
    CHECK(first.has_value());
    CHECK(second.has_value());
    if (!first || !second)
        return;
    auto trace = std::make_shared<Trace>();
    trace->backing = make_card("session-independent.img", 2);
    const auto factory = factory_for(trace);
    open_and_close(**first, factory, "/dev/session-a");
    open_and_close(**second, factory, "/dev/session-a");
    CHECK(trace->connections == 2U);
    CHECK(AuthorizationSessionAccess::state(**first) != AuthorizationSessionAccess::state(**second));
    first->reset();
    CHECK(trace->destroyed == 1U);
    open_and_close(**second, factory, "/dev/session-b");
    CHECK(trace->connections == 2U);
    CHECK(trace->destroyed == 1U);
    second->reset();
    CHECK(trace->destroyed == 2U);
}

void test_failed_exchange_retires_connection_without_retry() {
    for (const auto failure :
         {Failure::timeout, Failure::cancelled, Failure::denied, Failure::protocol, Failure::wrong_access}) {
        for (const auto stage : {Stage::resolve, Stage::open}) {
            if (failure == Failure::wrong_access && stage == Stage::resolve)
                continue;
            auto session = create_authorization_session();
            CHECK(session.has_value());
            if (!session)
                continue;
            auto trace = std::make_shared<Trace>();
            trace->backing = make_card("session-failure.img", 2);
            const auto factory = factory_for(trace);
            open_and_close(**session, factory, "/dev/session-before-failure");
            trace->failure = FailurePlan{failure, stage};
            const auto failed =
                linux_authorized_open("/dev/session-failing", Access::ReadOnly, **session, deadline(), factory);
            CHECK(err(failed) == failure_error(failure));
            CHECK(trace->connections == 1U);
            CHECK(trace->destroyed == 1U);
            CHECK(trace->resolve_calls == 2U);
            CHECK(trace->open_calls == (stage == Stage::resolve ? 1U : 2U));
            CHECK(!trace->failure);

            open_and_close(**session, factory, "/dev/session-explicit-retry");
            CHECK(trace->connections == 2U);
            CHECK(trace->destroyed == 1U);
            CHECK(trace->resolve_calls == 3U);
            CHECK(trace->open_calls == (stage == Stage::resolve ? 2U : 3U));
            session->reset();
            CHECK(trace->destroyed == 2U);
        }
    }
}

void test_queued_late_reply_descriptor_released_on_failure() {
    auto session = create_authorization_session();
    CHECK(session.has_value());
    if (!session)
        return;
    auto trace = std::make_shared<Trace>();
    trace->backing = make_card("late-reply-lock.img", 2);
    const auto factory = factory_for(trace);
    open_and_close(**session, factory, "/dev/session-before-timeout");
    trace->failure = FailurePlan{Failure::timeout, Stage::open, true};
    const auto failed = linux_authorized_open("/dev/session-timeout", Access::ReadOnly, **session, deadline(), factory);
    CHECK(err(failed) == Errc::authorization_timeout);
    CHECK(trace->queued_lock_observed);
    CHECK(trace->connections == 1U);
    CHECK(trace->destroyed == 1U);
    {
        PosixFd contender(::open(trace->backing.c_str(), O_RDWR | O_CLOEXEC));
        CHECK(contender.valid());
        CHECK(::flock(contender.get(), LOCK_EX | LOCK_NB) == 0);
    }
    open_and_close(**session, factory, "/dev/session-after-timeout");
    CHECK(trace->connections == 2U);
    CHECK(trace->destroyed == 1U);
    session->reset();
    CHECK(trace->destroyed == 2U);
}

} // namespace

int main() {
    CHECK(dbus_threads_init_default());
    test_connection_reused_across_workers_and_empty_device_gap();
    test_contexts_do_not_share_connections();
    test_failed_exchange_retires_connection_without_retry();
    test_queued_late_reply_descriptor_released_on_failure();
    return finish_tests();
}
