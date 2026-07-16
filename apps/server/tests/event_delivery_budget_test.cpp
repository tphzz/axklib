#include <gtest/gtest.h>

#include "axklib/server/event_delivery_budget.hpp"

TEST(EventDeliveryBudget, BoundsBothEventCountAndBytes) {
    axk::server::EventDeliveryBudget events{2U, 10U};
    EXPECT_TRUE(events.admit(4U));
    EXPECT_TRUE(events.admit(6U));
    EXPECT_FALSE(events.admit(1U));
    EXPECT_EQ(events.events(), 2U);
    EXPECT_EQ(events.bytes(), 10U);

    axk::server::EventDeliveryBudget bytes{10U, 5U};
    EXPECT_TRUE(bytes.admit(5U));
    EXPECT_FALSE(bytes.admit(1U));
}

TEST(EventDeliveryBudget, RejectsAnIndividualOversizeMessageWithoutConsumingCapacity) {
    axk::server::EventDeliveryBudget budget{2U, 8U};
    EXPECT_FALSE(budget.admit(9U));
    EXPECT_EQ(budget.events(), 0U);
    EXPECT_EQ(budget.bytes(), 0U);
    EXPECT_TRUE(budget.admit(8U));
}
