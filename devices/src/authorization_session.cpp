#include "authorization_session.hpp"
#include "detail.hpp"

#include <utility>

rawdisk::Result<std::shared_ptr<rawdisk::AuthorizationSession>>
rawdisk::create_authorization_session(const ElevationOptions &options) noexcept {
    return detail::guard([&]() -> Result<std::shared_ptr<AuthorizationSession>> {
        return std::make_shared<AuthorizationSession>(AuthorizationSession::Private{}, options);
    });
}

rawdisk::Result<std::unique_lock<std::timed_mutex>>
rawdisk::detail::lock_authorization(AuthorizationSession &session, std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline)
        return fail(Errc::authorization_timeout);
    std::unique_lock lock{AuthorizationSessionAccess::mutex(session), std::defer_lock};
    if (!lock.try_lock_until(deadline) || std::chrono::steady_clock::now() >= deadline)
        return fail(Errc::authorization_timeout);
    return lock;
}
