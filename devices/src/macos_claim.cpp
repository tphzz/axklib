#include "macos_claim.hpp"

#include "detail.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <IOKit/IOKitLib.h>
#include <sys/mount.h>

namespace rawdisk::detail {
namespace {

struct CFReleaseObject {
    void operator()(CFTypeRef value) const noexcept {
        if (value)
            CFRelease(value);
    }
};
template <class T> using CFObject = std::unique_ptr<std::remove_pointer_t<T>, CFReleaseObject>;

struct IOObject {
    io_object_t value{};
    explicit IOObject(io_object_t object = IO_OBJECT_NULL) : value(object) {}
    ~IOObject() {
        if (value != IO_OBJECT_NULL)
            IOObjectRelease(value);
    }
    IOObject(IOObject &&other) noexcept : value(std::exchange(other.value, IO_OBJECT_NULL)) {}
    IOObject(const IOObject &) = delete;
    IOObject &operator=(const IOObject &) = delete;
};

Result<bool> matches_disk(DADiskRef disk, const DeviceInfo &info) {
    std::uint64_t expected{};
    const auto parsed =
        std::from_chars(info.connection_id.data(), info.connection_id.data() + info.connection_id.size(), expected);
    if (parsed.ec != std::errc{} || parsed.ptr != info.connection_id.data() + info.connection_id.size())
        return fail(Errc::device_changed);
    IOObject media{DADiskCopyIOMedia(disk)};
    if (!media.value)
        return fail(Errc::device_changed);

    // Walk all parents: APFS volumes may belong to a synthesized whole disk.
    std::vector<IOObject> pending;
    pending.emplace_back(std::move(media));
    std::set<std::uint64_t> visited;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        std::uint64_t identity{};
        if (IORegistryEntryGetRegistryEntryID(current.value, &identity) != KERN_SUCCESS)
            return fail(Errc::device_changed);
        if (identity == expected)
            return true;
        if (!visited.insert(identity).second)
            continue;
        if (visited.size() > 256U)
            return fail(Errc::device_changed);
        IOObject parents;
        if (IORegistryEntryGetParentIterator(current.value, kIOServicePlane, &parents.value) != KERN_SUCCESS)
            return fail(Errc::device_changed);
        while (auto parent = IOIteratorNext(parents.value)) {
            IOObject retained{parent};
            if (pending.size() >= 256U)
                return fail(Errc::device_changed);
            pending.emplace_back(std::move(retained));
        }
    }
    return false;
}

Result<void> check_unmounted(DASessionRef session, const DeviceInfo &info) {
    for (unsigned attempt = 0; attempt != 4; ++attempt) {
        const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
        if (count < 0 || count > 4096)
            return fail(Errc::device_busy);
        std::vector<struct statfs> mounts(static_cast<std::size_t>(count) + 8U);
        const int got = ::getfsstat(mounts.data(), static_cast<int>(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
        if (got < 0)
            return fail(Errc::device_busy);
        if (static_cast<std::size_t>(got) >= mounts.size())
            continue;
        mounts.resize(static_cast<std::size_t>(got));
        for (const auto &mount : mounts) {
            if (!std::string_view(mount.f_mntfromname).starts_with("/dev/"))
                continue;
            CFObject<DADiskRef> mounted{DADiskCreateFromBSDName(nullptr, session, mount.f_mntfromname)};
            if (!mounted)
                return fail(Errc::device_busy);
            auto matches = matches_disk(mounted.get(), info);
            if (!matches || *matches)
                return fail(Errc::device_busy);
        }
        return {};
    }
    return fail(Errc::device_busy);
}

std::error_code claim_error(DADissenterRef dissenter) {
    if (!dissenter)
        return {};
    switch (DADissenterGetStatus(dissenter)) {
    case kDAReturnBusy:
    case kDAReturnExclusiveAccess:
        return make_error_code(Errc::device_busy);
    case kDAReturnNotWritable:
        return make_error_code(Errc::write_protected);
    case kDAReturnNotFound:
        return make_error_code(Errc::device_changed);
    case kDAReturnNotPermitted:
    case kDAReturnNotPrivileged:
        return make_error_code(Errc::authorization_denied);
    default:
        return make_error_code(Errc::internal_error);
    }
}

} // namespace

struct MacosWriteClaim::State {
    DeviceInfo info;
    MacosClaimApi api;
    CFObject<DADissenterRef> refusal;
    std::atomic<bool> stopping{};
    std::atomic<bool> active{};
    std::atomic<bool> lost{};
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::error_code> outcome;
    bool completed{};
    std::error_code completion_error;
    std::thread worker;

    State(DeviceInfo device, MacosClaimApi functions) : info(std::move(device)), api(functions) {}
    ~State() {
        stopping = true;
        if (worker.joinable())
            worker.join();
    }

    void publish(std::error_code error) {
        const std::scoped_lock lock(mutex);
        if (!outcome) {
            outcome = error;
            condition.notify_one();
        }
    }

    static void claimed(DADiskRef, DADissenterRef dissenter, void *context) noexcept {
        auto &state = *static_cast<State *>(context);
        state.completed = true;
        state.completion_error = claim_error(dissenter);
    }

    static DADissenterRef busy(State &state) noexcept {
        static_cast<void>(CFRetain(state.refusal.get()));
        return state.refusal.get();
    }

    static DADissenterRef release(DADiskRef, void *context) noexcept { return busy(*static_cast<State *>(context)); }

    static DADissenterRef approve(DADiskRef disk, void *context) noexcept {
        auto &state = *static_cast<State *>(context);
        try {
            const auto matches = state.api.matches(disk, state.info);
            if (matches && !*matches)
                return nullptr;
            if (!matches)
                state.lost = true;
        } catch (...) {
            state.lost = true;
        }
        return busy(state);
    }

    static void disappeared(DADiskRef disk, void *context) noexcept {
        auto &state = *static_cast<State *>(context);
        try {
            const auto matches = state.api.matches(disk, state.info);
            if (!matches || *matches)
                state.lost = true;
        } catch (...) {
            state.lost = true;
        }
    }

    static DADissenterRef approve_eject(DADiskRef disk, void *context) noexcept { return approve(disk, context); }

    struct Registration {
        State &state;
        CFObject<DASessionRef> session;
        CFObject<DADiskRef> disk;
        bool scheduled{};
        bool requested{};
        ~Registration() {
            state.active = false;
            if (scheduled) {
                state.api.unregister_callback(session.get(), reinterpret_cast<void *>(&approve), &state);
                state.api.unregister_callback(session.get(), reinterpret_cast<void *>(&approve_eject), &state);
                state.api.unregister_callback(session.get(), reinterpret_cast<void *>(&disappeared), &state);
                if (requested)
                    state.api.unclaim(disk.get());
                state.api.unschedule(session.get(), CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            }
        }
    };

    void run() noexcept {
        try {
            Registration registration{*this, CFObject<DASessionRef>{api.create_session(nullptr)}, {}, false, false};
            if (!registration.session) {
                publish(make_error_code(Errc::authorization_unavailable));
                return;
            }
            const auto bsd = info.path.substr(6);
            registration.disk.reset(api.create_disk(nullptr, registration.session.get(), bsd.c_str()));
            if (!registration.disk) {
                publish(make_error_code(Errc::device_changed));
                return;
            }
            auto session = registration.session.get();
            api.register_mount(session, nullptr, &approve, this);
            api.register_eject(session, nullptr, &approve_eject, this);
            api.register_disappeared(session, nullptr, &disappeared, this);
            api.schedule(session, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            registration.scheduled = true;
            registration.requested = true;
            api.claim(registration.disk.get(), kDADiskClaimOptionDefault, &release, this, &claimed, this);
            try {
                bool published{};
                while (!stopping) {
                    const auto status = api.service(kCFRunLoopDefaultMode, 0.01, true);
                    if (!published && completed) {
                        auto error = completion_error;
                        if (!error) {
                            const auto matches = api.matches(registration.disk.get(), info);
                            if (!matches || !*matches || lost)
                                error = make_error_code(Errc::device_changed);
                            else if (auto unmounted = api.check_unmounted(session, info); !unmounted)
                                error = unmounted.error();
                        }
                        active = !error;
                        publish(error);
                        published = true;
                    }
                    if (status == kCFRunLoopRunFinished)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } catch (...) {
                lost = true;
                publish(make_error_code(Errc::internal_error));
                // A failed callback service must not release ownership under an open descriptor.
                while (!stopping)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (...) {
            lost = true;
            publish(make_error_code(Errc::internal_error));
        }
        active = false;
    }
};

MacosWriteClaim::MacosWriteClaim(std::unique_ptr<State> state) : state_(std::move(state)) {}
MacosWriteClaim::~MacosWriteClaim() = default;
bool MacosWriteClaim::valid() const noexcept { return state_->active && !state_->lost && !state_->stopping; }

Result<std::unique_ptr<MacosWriteClaim>> MacosWriteClaim::acquire(const DeviceInfo &info, MacosClaimApi api,
                                                                  std::chrono::milliseconds timeout) {
    const std::string_view path = info.path;
    if (!path.starts_with("/dev/rdisk") || path.size() <= 10U ||
        path.find_first_not_of("0123456789", 10U) != std::string_view::npos || timeout.count() <= 0)
        return fail(Errc::invalid_argument);
    if (!api.matches)
        api.matches = &matches_disk;
    if (!api.check_unmounted)
        api.check_unmounted = &check_unmounted;
    auto claim = std::unique_ptr<MacosWriteClaim>(new MacosWriteClaim(std::make_unique<State>(info, api)));
    auto &state = *claim->state_;
    state.refusal.reset(DADissenterCreate(nullptr, kDAReturnBusy, CFSTR("axklib owns this disk for writing")));
    if (!state.refusal)
        return fail(std::make_error_code(std::errc::not_enough_memory));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    state.worker = std::thread([&state] { state.run(); });
    std::unique_lock lock(state.mutex);
    if (!state.condition.wait_until(lock, deadline, [&] { return state.outcome.has_value(); }))
        return fail(std::make_error_code(std::errc::timed_out));
    if (*state.outcome)
        return fail(*state.outcome);
    return claim;
}

} // namespace rawdisk::detail
