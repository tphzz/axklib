#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"

namespace axk::app {

enum class PathAccessMode : std::uint8_t { shared, exclusive };

struct PathAccess {
    FileRef reference;
    PathAccessMode mode{PathAccessMode::shared};
    bool all_roots{};
};

class PathReservationCoordinator {
  private:
    struct State;

  public:
    class Lease {
      public:
        Lease() = default;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        ~Lease();

        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

      private:
        friend class PathReservationCoordinator;
        Lease(std::shared_ptr<State> state, std::vector<std::uint64_t> reservations)
            : state_(std::move(state)), reservations_(std::move(reservations)) {}

        std::shared_ptr<State> state_;
        std::vector<std::uint64_t> reservations_;
    };

    PathReservationCoordinator();

    [[nodiscard]] Result<Lease> try_acquire(std::span<const PathAccess> accesses);
    [[nodiscard]] Result<Lease> try_acquire(PathAccess access);

  private:
    std::shared_ptr<State> state_;
};

} // namespace axk::app
