#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "conversion_plan_store.hpp"

namespace {
using namespace axk::app::conversion_internal;
std::shared_ptr<Payload> payload(std::uint64_t bytes = 50U) {
    axk::conversion::Report report;
    report.executable = true;
    auto value = std::make_shared<Payload>();
    value->prepared = std::make_shared<axk::conversion::PreparedPlan>(std::move(report),
                                                                      std::vector<axk::conversion::Patch>{}, 0U, bytes);
    return value;
}
} // namespace

TEST(ConversionPlanStore, AccountsForPendingPreparationAndRetainedPayloads) {
    PlanStore store{{4U, 100U, 200U, std::chrono::minutes{15}}};
    auto first = store.reserve("one");
    auto second = store.reserve("two");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(store.reserve("three"));
    EXPECT_FALSE(store.publish(*first, "other", payload()));
    ASSERT_TRUE(store.publish(*first, "one", payload()));
    EXPECT_FALSE(store.reserve("three"));
    store.abandon(*second, "two");
    auto third = store.reserve("three");
    EXPECT_TRUE(third);
    EXPECT_FALSE(store.get(*first, "two"));
    EXPECT_FALSE(store.release(*first, "two"));
    EXPECT_TRUE(store.release(*first, "one"));
    EXPECT_FALSE(store.get(*first, "one"));
}

TEST(ConversionPlanStore, RetainsExactlyOneAcceptedJobAndReleasesTerminalPayload) {
    PlanStore store;
    auto id = store.reserve("owner");
    ASSERT_TRUE(id);
    auto value = payload();
    std::weak_ptr<const Payload> weak = value;
    ASSERT_TRUE(store.publish(*id, "owner", value));
    value.reset();
    std::size_t calls{};
    auto submit = [&]() -> axk::app::Result<std::string> {
        ++calls;
        return "job-one";
    };
    ASSERT_EQ(store.submit(*id, "owner", {}, submit).value(), "job-one");
    ASSERT_EQ(store.submit(*id, "owner", {}, submit).value(), "job-one");
    EXPECT_EQ(calls, 1U);
    EXPECT_FALSE(store.release(*id, "owner"));
    {
        auto claim = store.start(*id, "owner");
        ASSERT_TRUE(claim);
    }
    EXPECT_FALSE(store.start(*id, "owner"));
    store.finish(*id, "owner");
    EXPECT_TRUE(weak.expired());
    ASSERT_EQ(store.submit(*id, "owner", {}, submit).value(), "job-one");
    EXPECT_EQ(calls, 1U);
    EXPECT_FALSE(store.get(*id, "owner")->payload);
}

TEST(ConversionPlanStore, FailedSubmissionDoesNotConsumeButAcceptedCancellationDoes) {
    PlanStore store;
    auto id = store.reserve("owner");
    ASSERT_TRUE(id);
    ASSERT_TRUE(store.publish(*id, "owner", payload()));
    auto failed = store.submit(*id, "owner", {}, []() -> axk::app::Result<std::string> {
        return std::unexpected(axk::app::Error{"queue_full", "Full"});
    });
    EXPECT_FALSE(failed);
    EXPECT_EQ(store.get(*id, "owner")->state, State::ready);
    ASSERT_TRUE(store.submit(*id, "owner", {}, []() -> axk::app::Result<std::string> { return "job"; }));
    store.finish(*id, "owner");
    EXPECT_FALSE(store.start(*id, "owner"));
    EXPECT_EQ(store.get(*id, "owner")->job_id, "job");
}

TEST(ConversionPlanStore, ExpiresReadyPlansButNotAcceptedExecutions) {
    auto now = PlanStore::Clock::now();
    PlanStore store{{4U, 100U, 200U, std::chrono::seconds{1}}, [&] { return now; }};
    auto id = store.reserve("owner");
    ASSERT_TRUE(id);
    ASSERT_TRUE(store.publish(*id, "owner", payload()));
    now += std::chrono::seconds{2};
    EXPECT_FALSE(store.get(*id, "owner"));
    id = store.reserve("owner");
    ASSERT_TRUE(id);
    ASSERT_TRUE(store.publish(*id, "owner", payload()));
    ASSERT_TRUE(store.submit(*id, "owner", {}, []() -> axk::app::Result<std::string> { return "job"; }));
    now += std::chrono::seconds{2};
    EXPECT_TRUE(store.start(*id, "owner"));
    store.finish(*id, "owner");
    now += std::chrono::seconds{2};
    EXPECT_FALSE(store.get(*id, "owner"));
}

TEST(ConversionPlanStore, ChargesSourceLeasesAndCollectsWithoutAClientRequest) {
    auto now = PlanStore::Clock::now();
    PlanStore store{{4U, 100U, 200U, std::chrono::seconds{1}}, [&] { return now; }};
    auto id = store.reserve("owner");
    ASSERT_TRUE(id);
    auto value = payload(50U);
    value->source_retention_bytes = 51U;
    EXPECT_FALSE(store.publish(*id, "owner", value));
    value->source_retention_bytes = 50U;
    ASSERT_TRUE(store.publish(*id, "owner", value));
    std::weak_ptr<const Payload> weak = value;
    value.reset();
    now += std::chrono::seconds{2};
    store.collect_expired();
    EXPECT_TRUE(weak.expired());
}
