#include "macos_claim.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace rawdisk;
using namespace rawdisk::detail;
using namespace std::chrono_literals;

namespace {

struct Fake {
    std::mutex mutex;
    std::vector<std::function<void()>> pending;
    DADiskRef disk{};
    DADiskRef unrelated_disk = reinterpret_cast<DADiskRef>(CFDictionaryCreateMutable(nullptr, 0, nullptr, nullptr));
    DADiskMountApprovalCallback mount{};
    DADiskEjectApprovalCallback eject{};
    DADiskDisappearedCallback disappeared{};
    DADiskClaimReleaseCallback release{};
    void *context{};
    std::atomic<unsigned> services{};
    unsigned unclaims{};
    unsigned unschedules{};
    unsigned unregistrations{};
    bool mounted{};
    bool mount_check_failed{};
    bool match_failed{};
    bool session_failed{};
    bool disk_failed{};
    bool complete = true;
    DAReturn claim_status = kDAReturnSuccess;

    ~Fake() {
        if (unrelated_disk)
            CFRelease(unrelated_disk);
    }
};

Fake *fake{};

MacosClaimApi api() {
    MacosClaimApi result;
    result.create_session = [](CFAllocatorRef allocator) -> DASessionRef {
        if (fake->session_failed)
            return nullptr;
        return reinterpret_cast<DASessionRef>(CFDictionaryCreateMutable(allocator, 0, nullptr, nullptr));
    };
    result.create_disk = [](CFAllocatorRef allocator, DASessionRef, const char *) -> DADiskRef {
        if (fake->disk_failed)
            return nullptr;
        fake->disk = reinterpret_cast<DADiskRef>(CFDictionaryCreateMutable(allocator, 0, nullptr, nullptr));
        return fake->disk;
    };
    result.schedule = [](DASessionRef, CFRunLoopRef, CFStringRef) {};
    result.unschedule = [](DASessionRef, CFRunLoopRef, CFStringRef) { ++fake->unschedules; };
    result.register_mount = [](DASessionRef, CFDictionaryRef, DADiskMountApprovalCallback callback, void *context) {
        fake->mount = callback;
        fake->context = context;
    };
    result.register_eject = [](DASessionRef, CFDictionaryRef, DADiskEjectApprovalCallback callback, void *) {
        fake->eject = callback;
    };
    result.register_disappeared = [](DASessionRef, CFDictionaryRef, DADiskDisappearedCallback callback, void *) {
        fake->disappeared = callback;
    };
    result.unregister_callback = [](DASessionRef, void *, void *) { ++fake->unregistrations; };
    result.claim = [](DADiskRef disk, DADiskClaimOptions, DADiskClaimReleaseCallback release, void *context,
                      DADiskClaimCallback callback, void *) {
        fake->release = release;
        if (!fake->complete)
            return;
        const auto status = fake->claim_status;
        std::scoped_lock lock(fake->mutex);
        fake->pending.emplace_back([disk, callback, context, status] {
            auto dissenter = status == kDAReturnSuccess ? nullptr : DADissenterCreate(nullptr, status, nullptr);
            callback(disk, dissenter, context);
            if (dissenter)
                CFRelease(dissenter);
        });
    };
    result.unclaim = [](DADiskRef) { ++fake->unclaims; };
    using RunResult = std::invoke_result_t<decltype(result.service), CFStringRef, CFTimeInterval, Boolean>;
    result.service = [](CFStringRef, CFTimeInterval, Boolean) -> RunResult {
        std::vector<std::function<void()>> pending;
        {
            std::scoped_lock lock(fake->mutex);
            pending.swap(fake->pending);
        }
        for (auto &event : pending)
            event();
        ++fake->services;
        std::this_thread::sleep_for(1ms);
        return static_cast<RunResult>(kCFRunLoopRunHandledSource);
    };
    result.matches = [](DADiskRef disk, const DeviceInfo &) -> Result<bool> {
        if (fake->match_failed)
            return std::unexpected(make_error_code(Errc::device_changed));
        return disk == fake->disk;
    };
    result.check_unmounted = [](DASessionRef, const DeviceInfo &) -> Result<void> {
        if (fake->mount_check_failed)
            return std::unexpected(std::make_error_code(std::errc::io_error));
        if (fake->mounted)
            return std::unexpected(make_error_code(Errc::device_busy));
        return {};
    };
    return result;
}

DeviceInfo device() { return {.path = "/dev/rdisk987", .connection_id = "42"}; }

void service_event(std::function<void()> event) {
    struct Completion {
        std::mutex mutex;
        std::condition_variable condition;
        bool done{};
    };
    auto completion = std::make_shared<Completion>();
    {
        std::scoped_lock lock(fake->mutex);
        fake->pending.emplace_back([completion, event = std::move(event)] {
            event();
            const std::scoped_lock completed(completion->mutex);
            completion->done = true;
            completion->condition.notify_one();
        });
    }
    std::unique_lock lock(completion->mutex);
    CHECK(completion->condition.wait_for(lock, 2s, [&] { return completion->done; }));
}

void lifecycle() {
    Fake state;
    fake = &state;
    CHECK(state.unrelated_disk);
    if (!state.unrelated_disk)
        return;
    auto acquired = MacosWriteClaim::acquire(device(), api(), 1s);
    CHECK(acquired);
    if (!acquired)
        return;
    auto claim = std::move(*acquired);
    CHECK(claim->valid());
    const auto services = state.services.load();
    service_event([&] {
        CHECK(state.services.load() >= services);
        for (auto callback : {state.mount, state.eject, state.release}) {
            auto denied = callback(state.disk, state.context);
            CHECK(denied);
            if (denied) {
                CHECK(DADissenterGetStatus(denied) == kDAReturnBusy);
                CFRelease(denied);
            }
        }
        CHECK(state.mount(state.unrelated_disk, state.context) == nullptr);
    });
    CHECK(state.unclaims == 0U);
    claim.reset();
    CHECK(state.unclaims == 1U);
    CHECK(state.unschedules == 1U);
    CHECK(state.unregistrations == 3U);
}

void failures() {
    for (auto status : {kDAReturnBusy, kDAReturnExclusiveAccess, kDAReturnNotPermitted}) {
        Fake state;
        fake = &state;
        state.claim_status = status;
        auto result = MacosWriteClaim::acquire(device(), api(), 1s);
        CHECK(!result);
        CHECK(state.unclaims == 1U);
        CHECK(state.unregistrations == 3U);
    }
    for (unsigned failure = 0; failure != 5; ++failure) {
        Fake state;
        fake = &state;
        state.session_failed = failure == 0;
        state.disk_failed = failure == 1;
        state.mounted = failure == 2;
        state.mount_check_failed = failure == 3;
        state.match_failed = failure == 4;
        CHECK(!MacosWriteClaim::acquire(device(), api(), 1s));
        CHECK(state.unclaims == (failure < 2 ? 0U : 1U));
    }
    Fake state;
    fake = &state;
    state.complete = false;
    CHECK(err(MacosWriteClaim::acquire(device(), api(), 10ms)) == std::errc::timed_out);
    CHECK(state.unclaims == 1U);
    CHECK(state.unschedules == 1U);
}

void disappearance_and_uncertain_matching() {
    for (bool uncertain : {false, true}) {
        Fake state;
        fake = &state;
        CHECK(state.unrelated_disk);
        if (!state.unrelated_disk)
            continue;
        auto claim = MacosWriteClaim::acquire(device(), api(), 1s);
        CHECK(claim);
        if (!claim)
            continue;
        service_event([&] {
            if (uncertain) {
                state.match_failed = true;
                auto denied = state.mount(state.disk, state.context);
                CHECK(denied);
                if (denied)
                    CFRelease(denied);
            } else {
                state.disappeared(state.unrelated_disk, state.context);
                CHECK((*claim)->valid());
                state.disappeared(state.disk, state.context);
            }
        });
        CHECK(!(*claim)->valid());
    }
}

void failed_service_retains_ownership_until_close() {
    Fake state;
    fake = &state;
    auto claim = MacosWriteClaim::acquire(device(), api(), 1s);
    CHECK(claim);
    if (!claim)
        return;
    {
        const std::scoped_lock lock(state.mutex);
        state.pending.emplace_back([] { throw std::runtime_error("injected service failure"); });
    }
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while ((*claim)->valid() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(!(*claim)->valid());
    CHECK(state.unclaims == 0U);
    claim->reset();
    CHECK(state.unclaims == 1U);
    CHECK(state.unregistrations == 3U);
}

} // namespace

int main() {
    lifecycle();
    failures();
    disappearance_and_uncertain_matching();
    failed_service_retains_ownership_until_close();
    return finish_tests();
}
