#pragma once

#include <cstddef>
#include <cstdint>

namespace axk::server {

class EventDeliveryBudget {
  public:
    EventDeliveryBudget(std::size_t maximum_events, std::uint64_t maximum_bytes) noexcept;

    [[nodiscard]] bool admit(std::size_t message_bytes) noexcept;
    [[nodiscard]] std::size_t events() const noexcept { return events_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

  private:
    std::size_t maximum_events_{};
    std::uint64_t maximum_bytes_{};
    std::size_t events_{};
    std::uint64_t bytes_{};
};

} // namespace axk::server
