#include <chrono>

#include <gtest/gtest.h>

#include "axklib/server/event_tickets.hpp"

namespace {

using namespace std::chrono_literals;

TEST(EventTicketStore, IssuesSingleUseOwnedTickets) {
    axk::server::EventTicketStore tickets{30s, 2U};
    const auto issued = tickets.issue("principal");
    ASSERT_TRUE(issued) << issued.error().message;
    EXPECT_EQ(issued->expires_in_seconds, 30U);
    EXPECT_EQ(issued->ticket_id.size(), 64U);

    const auto consumed = tickets.consume(issued->ticket_id);
    ASSERT_TRUE(consumed) << consumed.error().message;
    EXPECT_EQ(*consumed, "principal");
    const auto reused = tickets.consume(issued->ticket_id);
    ASSERT_FALSE(reused);
    EXPECT_EQ(reused.error().code, "event_ticket_invalid");
}

TEST(EventTicketStore, ExpiresTicketsAndReclaimsCapacity) {
    auto now = axk::server::EventTicketStore::Clock::time_point{};
    axk::server::EventTicketStore tickets{5s, 1U, [&] { return now; }};
    const auto first = tickets.issue("principal");
    ASSERT_TRUE(first);
    const auto full = tickets.issue("principal");
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, "event_ticket_capacity_exhausted");
    EXPECT_TRUE(full.error().retryable);

    now += 5s;
    const auto expired = tickets.consume(first->ticket_id);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "event_ticket_invalid");
    EXPECT_TRUE(tickets.issue("principal"));
}

TEST(EventTicketStore, RejectsOwnerlessTickets) {
    axk::server::EventTicketStore tickets{30s, 1U};
    const auto issued = tickets.issue({});
    ASSERT_FALSE(issued);
    EXPECT_EQ(issued.error().code, "invalid_ticket_owner");
}

} // namespace
