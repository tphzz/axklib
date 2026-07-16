#include "axklib/server/event_delivery_budget.hpp"

#include <limits>

axk::server::EventDeliveryBudget::EventDeliveryBudget(std::size_t maximum_events, std::uint64_t maximum_bytes) noexcept
    : maximum_events_(maximum_events), maximum_bytes_(maximum_bytes) {}

bool axk::server::EventDeliveryBudget::admit(std::size_t message_bytes) noexcept {
    if (events_ >= maximum_events_ || message_bytes > std::numeric_limits<std::uint64_t>::max() - bytes_ ||
        bytes_ + message_bytes > maximum_bytes_) {
        return false;
    }
    ++events_;
    bytes_ += message_bytes;
    return true;
}
