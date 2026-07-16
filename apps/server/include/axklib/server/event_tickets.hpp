#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "axklib/application/contracts.hpp"

namespace axk::server {

struct EventTicket {
    std::string ticket_id;
    std::uint32_t expires_in_seconds{};
};

class EventTicketStore {
  public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    EventTicketStore(
        std::chrono::seconds time_to_live, std::size_t maximum_tickets, Now now = [] { return Clock::now(); });

    [[nodiscard]] app::Result<EventTicket> issue(std::string owner_id);
    [[nodiscard]] app::Result<std::string> consume(std::string_view ticket_id);

  private:
    struct Record {
        std::string owner_id;
        Clock::time_point expires_at;
    };

    void remove_expired_locked(Clock::time_point now);
    [[nodiscard]] app::Result<std::string> next_id();

    const std::chrono::seconds time_to_live_;
    const std::size_t maximum_tickets_;
    Now now_;
    std::mutex mutex_;
    std::unordered_map<std::string, Record> tickets_;
};

} // namespace axk::server
