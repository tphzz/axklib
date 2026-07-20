#include <gtest/gtest.h>

#include "archive_download_budget.hpp"

TEST(ArchiveDownloadBudget, BoundsActiveTransfersAndReleasesWithLeaseLifetime) {
    axk::server::ArchiveDownloadBudget budget{2U};
    auto first = budget.try_acquire();
    auto second = budget.try_acquire();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(budget.active(), 2U);
    EXPECT_FALSE(budget.try_acquire());

    first.reset();
    EXPECT_EQ(budget.active(), 1U);
    EXPECT_TRUE(budget.try_acquire());
}
