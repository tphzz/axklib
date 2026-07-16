#include "axklib/server/event_tickets.hpp"

#include <utility>

#include "axklib/application/secure_random.hpp"

namespace {

axk::app::Error ticket_error(std::string code, std::string message, bool retryable = false) {
    return {std::move(code), std::move(message), {}, retryable};
}

} // namespace

axk::server::EventTicketStore::EventTicketStore(std::chrono::seconds time_to_live, std::size_t maximum_tickets, Now now)
    : time_to_live_(time_to_live), maximum_tickets_(maximum_tickets), now_(std::move(now)) {}

axk::app::Result<axk::server::EventTicket> axk::server::EventTicketStore::issue(std::string owner_id) {
    if (owner_id.empty())
        return std::unexpected(ticket_error("invalid_ticket_owner", "event ticket owner must not be empty"));

    const std::scoped_lock lock{mutex_};
    const auto now = now_();
    remove_expired_locked(now);
    if (tickets_.size() >= maximum_tickets_)
        return std::unexpected(
            ticket_error("event_ticket_capacity_exhausted", "event ticket capacity is exhausted", true));

    std::string ticket_id;
    do {
        auto generated = next_id();
        if (!generated)
            return std::unexpected(generated.error());
        ticket_id = std::move(*generated);
    } while (tickets_.contains(ticket_id));
    tickets_.emplace(ticket_id, Record{std::move(owner_id), now + time_to_live_});
    return EventTicket{std::move(ticket_id), static_cast<std::uint32_t>(time_to_live_.count())};
}

axk::app::Result<std::string> axk::server::EventTicketStore::consume(std::string_view ticket_id) {
    const std::scoped_lock lock{mutex_};
    const auto now = now_();
    remove_expired_locked(now);
    const auto found = tickets_.find(std::string{ticket_id});
    if (found == tickets_.end())
        return std::unexpected(ticket_error("event_ticket_invalid", "event ticket is expired, used, or unknown"));
    auto owner_id = std::move(found->second.owner_id);
    tickets_.erase(found);
    return owner_id;
}

void axk::server::EventTicketStore::remove_expired_locked(Clock::time_point now) {
    for (auto iterator = tickets_.begin(); iterator != tickets_.end();) {
        if (iterator->second.expires_at <= now)
            iterator = tickets_.erase(iterator);
        else
            ++iterator;
    }
}

axk::app::Result<std::string> axk::server::EventTicketStore::next_id() { return axk::app::secure_random_hex(32U); }
