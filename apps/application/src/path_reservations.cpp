#include "axklib/application/path_reservations.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace {

std::string normalize_path(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    for (const auto character : path) {
#if defined(_WIN32) || defined(__APPLE__)
        if ((static_cast<unsigned char>(character) & 0x80U) != 0U)
            return {};
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
#else
        result.push_back(character);
#endif
    }
    while (!result.empty() && result.back() == '/')
        result.pop_back();
    return result;
}

bool paths_overlap(std::string_view left, std::string_view right) {
    const auto contains = [](std::string_view parent, std::string_view child) {
        return parent.empty() || child == parent ||
               (child.size() > parent.size() && child.starts_with(parent) && child[parent.size()] == '/');
    };
    return contains(left, right) || contains(right, left);
}

axk::app::Error conflict_error() {
    return {"entry_in_use", "close open images and wait for active file operations to finish", {}, true};
}

} // namespace

struct axk::app::PathReservationCoordinator::State {
    struct Reservation {
        std::uint64_t id{};
        std::string root_id;
        std::string relative_path;
        PathAccessMode mode{PathAccessMode::shared};
        bool all_roots{};
    };

    void release(std::span<const std::uint64_t> ids) noexcept {
        const std::scoped_lock lock{mutex};
        std::erase_if(reservations, [&](const Reservation &reservation) {
            return std::ranges::find(ids, reservation.id) != ids.end();
        });
    }

    std::mutex mutex;
    std::vector<Reservation> reservations;
    std::uint64_t next_id{1U};
};

axk::app::PathReservationCoordinator::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)), reservations_(std::move(other.reservations_)) {}

axk::app::PathReservationCoordinator::Lease &
axk::app::PathReservationCoordinator::Lease::operator=(Lease &&other) noexcept {
    if (this == &other)
        return *this;
    if (state_)
        state_->release(reservations_);
    state_ = std::move(other.state_);
    reservations_ = std::move(other.reservations_);
    return *this;
}

axk::app::PathReservationCoordinator::Lease::~Lease() {
    if (state_)
        state_->release(reservations_);
}

axk::app::PathReservationCoordinator::PathReservationCoordinator() : state_(std::make_shared<State>()) {}

axk::app::Result<axk::app::PathReservationCoordinator::Lease>
axk::app::PathReservationCoordinator::try_acquire(std::span<const PathAccess> accesses) {
    std::vector<State::Reservation> requested;
    requested.reserve(accesses.size());
    for (const auto &access : accesses) {
        auto normalized = State::Reservation{.root_id = access.reference.root_id,
                                             .relative_path = normalize_path(access.reference.relative_path),
                                             .mode = access.mode,
                                             .all_roots = access.all_roots};
        const auto duplicate = std::ranges::find_if(requested, [&](const State::Reservation &candidate) {
            return candidate.root_id == normalized.root_id && candidate.relative_path == normalized.relative_path &&
                   candidate.all_roots == normalized.all_roots;
        });
        if (duplicate == requested.end()) {
            requested.push_back(std::move(normalized));
        } else if (access.mode == PathAccessMode::exclusive) {
            duplicate->mode = PathAccessMode::exclusive;
        }
    }

    const std::scoped_lock lock{state_->mutex};
    for (const auto &candidate : requested) {
        const auto conflict = std::ranges::any_of(state_->reservations, [&](const State::Reservation &active) {
            return (active.all_roots || candidate.all_roots || active.root_id == candidate.root_id) &&
                   paths_overlap(active.relative_path, candidate.relative_path) &&
                   (active.mode == PathAccessMode::exclusive || candidate.mode == PathAccessMode::exclusive);
        });
        if (conflict)
            return std::unexpected(conflict_error());
    }

    std::vector<std::uint64_t> ids;
    ids.reserve(requested.size());
    for (auto &reservation : requested) {
        reservation.id = state_->next_id++;
        ids.push_back(reservation.id);
        state_->reservations.push_back(std::move(reservation));
    }
    return Lease{state_, std::move(ids)};
}

axk::app::Result<axk::app::PathReservationCoordinator::Lease>
axk::app::PathReservationCoordinator::try_acquire(PathAccess access) {
    return try_acquire(std::span<const PathAccess>{&access, 1U});
}
