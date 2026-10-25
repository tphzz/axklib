#include "authorization_session.hpp"
#include "test_util.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

using namespace rawdisk;
using namespace rawdisk::detail;
using namespace std::chrono_literals;

namespace {

struct ObservedState final : AuthorizationState {
    explicit ObservedState(unsigned &destroyed) : destroyed_(destroyed) {}
    ~ObservedState() override { ++destroyed_; }

  private:
    unsigned &destroyed_;
};

void test_lazy_independent_sessions() {
    ElevationOptions options;
    options.allow = false;
    options.helper_path = "unused-helper";
    options.connect_timeout_ms = 17;
    auto first = create_authorization_session(options);
    auto second = create_authorization_session();
    CHECK(first.has_value());
    CHECK(second.has_value());
    if (!first || !second)
        return;
    CHECK(first->get() != second->get());
    CHECK(!AuthorizationSessionAccess::state(**first));
    CHECK(!AuthorizationSessionAccess::state(**second));
    CHECK(!AuthorizationSessionAccess::options(**first).allow);
    CHECK(AuthorizationSessionAccess::options(**second).allow);
    CHECK(AuthorizationSessionAccess::options(**first).helper_path == options.helper_path);
    CHECK(AuthorizationSessionAccess::options(**first).connect_timeout_ms == 17U);

    unsigned destroyed{};
    AuthorizationSessionAccess::state(**first) = std::make_shared<ObservedState>(destroyed);
    CHECK(!AuthorizationSessionAccess::state(**second));
    {
        auto first_lock = lock_authorization(**first, std::chrono::steady_clock::now() + 1s);
        auto second_lock = lock_authorization(**second, std::chrono::steady_clock::now() + 1s);
        CHECK(first_lock.has_value());
        CHECK(second_lock.has_value());
        CHECK(first_lock && first_lock->owns_lock());
        CHECK(second_lock && second_lock->owns_lock());
    }
    first->reset();
    CHECK(destroyed == 1U);
    CHECK(!AuthorizationSessionAccess::state(**second));
}

void test_state_released_at_last_session_owner() {
    unsigned destroyed{};
    auto created = create_authorization_session();
    CHECK(created.has_value());
    if (!created)
        return;
    auto session = std::move(*created);
    auto retained = session;
    auto state = std::make_shared<ObservedState>(destroyed);
    const std::weak_ptr<AuthorizationState> observed = state;
    AuthorizationSessionAccess::state(*session) = state;
    state.reset();
    session.reset();
    CHECK(destroyed == 0U);
    CHECK(!observed.expired());
    retained.reset();
    CHECK(destroyed == 1U);
    CHECK(observed.expired());
}

void test_lock_serializes_workers_and_honors_deadline() {
    auto session = create_authorization_session();
    CHECK(session.has_value());
    if (!session)
        return;

    std::promise<bool> acquired;
    auto acquired_result = acquired.get_future();
    std::promise<void> release;
    auto release_signal = release.get_future();
    std::thread owner([&] {
        auto lock = lock_authorization(**session, std::chrono::steady_clock::now() + 2s);
        acquired.set_value(lock.has_value() && lock->owns_lock());
        if (lock)
            (void)release_signal.wait_for(5s);
    });
    const bool owner_ready = acquired_result.wait_for(2s) == std::future_status::ready;
    CHECK(owner_ready);
    CHECK(owner_ready && acquired_result.get());

    std::promise<std::error_code> attempted;
    auto attempt_result = attempted.get_future();
    std::thread waiter([&] {
        const auto lock = lock_authorization(**session, std::chrono::steady_clock::now() + 50ms);
        attempted.set_value(err(lock));
    });
    const bool timed_out = attempt_result.wait_for(2s) == std::future_status::ready;
    CHECK(timed_out);
    CHECK(timed_out && attempt_result.get() == Errc::authorization_timeout);
    release.set_value();
    waiter.join();
    owner.join();

    bool acquired_after_release{};
    std::thread next([&] {
        const auto lock = lock_authorization(**session, std::chrono::steady_clock::now() + 1s);
        acquired_after_release = lock.has_value() && lock->owns_lock();
    });
    next.join();
    CHECK(acquired_after_release);
}

} // namespace

int main() {
    test_lazy_independent_sessions();
    test_state_released_at_last_session_owner();
    test_lock_serializes_workers_and_honors_deadline();
    return finish_tests();
}
