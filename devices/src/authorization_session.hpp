#pragma once

#include "rawdisk/block_device.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace rawdisk::detail {
struct AuthorizationState {
    virtual ~AuthorizationState() = default;
};
struct AuthorizationSessionAccess;
} // namespace rawdisk::detail

namespace rawdisk {

class AuthorizationSession {
    struct Private {};
    friend Result<std::shared_ptr<AuthorizationSession>>
    create_authorization_session(const ElevationOptions &) noexcept;
    friend struct detail::AuthorizationSessionAccess;

  public:
    AuthorizationSession(Private, ElevationOptions options) : options_(std::move(options)) {}
    AuthorizationSession(const AuthorizationSession &) = delete;
    AuthorizationSession &operator=(const AuthorizationSession &) = delete;

  private:
    ElevationOptions options_;
    std::timed_mutex mutex_;
    std::shared_ptr<detail::AuthorizationState> state_;
};

} // namespace rawdisk

namespace rawdisk::detail {

struct AuthorizationSessionAccess {
    static const ElevationOptions &options(const AuthorizationSession &session) { return session.options_; }
    static std::timed_mutex &mutex(AuthorizationSession &session) { return session.mutex_; }
    static std::shared_ptr<AuthorizationState> &state(AuthorizationSession &session) { return session.state_; }
};

Result<std::unique_lock<std::timed_mutex>> lock_authorization(AuthorizationSession &session,
                                                              std::chrono::steady_clock::time_point deadline);

} // namespace rawdisk::detail
