#include "axklib/application/jobs.hpp"
#include "jobs_test_access.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;

struct ReleaseGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool releasing{};
    bool release{};
    std::atomic_uint calls{};
    std::atomic_uint releases{};

    std::shared_ptr<void> acquire() {
        ++calls;
        return {new int{}, [this](int *value) {
                    delete value;
                    std::unique_lock lock{mutex};
                    releasing = true;
                    condition.notify_all();
                    condition.wait(lock, [&] { return release; });
                    ++releases;
                }};
    }

    bool wait() {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, 2s, [&] { return releasing; });
    }

    void finish() {
        const std::scoped_lock lock{mutex};
        release = true;
        condition.notify_all();
    }
};

axk::app::OperationRegistry admitted_registry(ReleaseGate &gate, std::atomic_bool *executing = nullptr) {
    axk::app::OperationRegistry registry;
    EXPECT_TRUE(registry.declare({"test.write",
                                  "Admitted write",
                                  axk::app::HttpMethod::post,
                                  "/api/v1/test-writes",
                                  axk::app::ExecutionMode::job,
                                  {},
                                  "Request",
                                  "Result",
                                  axk::app::OperationClass::write,
                                  true}));
    EXPECT_TRUE(registry.bind("test.write", [executing](const auto &, const auto &) {
        if (executing)
            *executing = true;
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    }));
    EXPECT_TRUE(registry.bind_job_admission("test.write", [&](const auto &, const auto &) {
        return axk::app::Result<std::shared_ptr<void>>{gate.acquire()};
    }));
    return registry;
}

TEST(JobAdmission, CompletionIsNotPublishedUntilItsReservationIsReleased) {
    ReleaseGate gate;
    auto registry = admitted_registry(gate);
    axk::app::JobManager jobs{registry, 1U, 1U, 2U, 8U};
    axk::app::OperationContext context{};
    context.owner_id = "owner";
    const auto submitted = jobs.submit("test.write", {}, context, "operation");
    ASSERT_TRUE(submitted);
    EXPECT_TRUE(gate.wait());
    const auto during = jobs.status(submitted->job_id, "owner");
    EXPECT_TRUE(during);
    if (during)
        EXPECT_FALSE(axk::app::is_terminal(during->state));
    gate.finish();
    jobs.shutdown();
    const auto after = jobs.status(submitted->job_id, "owner");
    ASSERT_TRUE(after);
    EXPECT_EQ(after->state, axk::app::JobState::completed);
}

TEST(JobAdmission, QueuedCancellationReleasesBeforePublishingAndCannotRun) {
    ReleaseGate gate;
    std::atomic_bool executing{};
    auto registry = admitted_registry(gate, &executing);
    axk::app::JobManager jobs{registry, 1U, 1U, 2U, 8U};
    std::mutex worker_mutex;
    std::condition_variable worker_condition;
    bool popped{};
    bool claim{};
    axk::app::JobManagerTestAccess::set_before_running_claim(jobs, [&] {
        std::unique_lock lock{worker_mutex};
        popped = true;
        worker_condition.notify_all();
        worker_condition.wait(lock, [&] { return claim; });
    });
    axk::app::OperationContext context{};
    context.owner_id = "owner";
    const auto submitted = jobs.submit("test.write", {}, context, "operation");
    ASSERT_TRUE(submitted);
    {
        std::unique_lock lock{worker_mutex};
        EXPECT_TRUE(worker_condition.wait_for(lock, 2s, [&] { return popped; }));
    }
    std::thread cancellation{[&] { EXPECT_TRUE(jobs.cancel(submitted->job_id, "owner")); }};
    EXPECT_TRUE(gate.wait());
    const auto during = jobs.status(submitted->job_id, "owner");
    EXPECT_TRUE(during);
    if (during)
        EXPECT_FALSE(axk::app::is_terminal(during->state));
    {
        const std::scoped_lock lock{worker_mutex};
        claim = true;
    }
    worker_condition.notify_all();
    gate.finish();
    cancellation.join();
    jobs.shutdown();
    const auto after = jobs.status(submitted->job_id, "owner");
    ASSERT_TRUE(after);
    EXPECT_EQ(after->state, axk::app::JobState::cancelled);
    EXPECT_FALSE(executing);
}

TEST(JobAdmission, IdempotentReplayDoesNotConsumeTheReservationAgain) {
    ReleaseGate gate;
    auto registry = admitted_registry(gate);
    axk::app::JobManager jobs{registry, 1U, 1U, 2U, 8U};
    axk::app::OperationContext context{};
    context.owner_id = "owner";
    const auto submitted = jobs.submit("test.write", {}, context, "operation");
    ASSERT_TRUE(submitted);
    EXPECT_TRUE(gate.wait());
    const auto replay = jobs.submit("test.write", {}, context, "operation");
    EXPECT_TRUE(replay);
    if (replay)
        EXPECT_EQ(replay->job_id, submitted->job_id);
    EXPECT_EQ(gate.calls, 1U);
    gate.finish();
    jobs.shutdown();
    EXPECT_EQ(gate.releases, 1U);
}

TEST(JobAdmission, QueueRefusalReleasesItsReservationBeforeReturning) {
    ReleaseGate gate;
    gate.finish();
    auto registry = admitted_registry(gate);
    axk::app::JobManager jobs{registry, 1U, 1U, 1U, 8U};
    std::mutex mutex;
    std::condition_variable condition;
    bool popped{};
    bool release{};
    axk::app::JobManagerTestAccess::set_before_running_claim(jobs, [&] {
        std::unique_lock lock{mutex};
        popped = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
    });
    axk::app::OperationContext context{};
    context.owner_id = "owner";
    const auto first = jobs.submit("test.write", {}, context, "one");
    ASSERT_TRUE(first);
    {
        std::unique_lock lock{mutex};
        EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return popped; }));
    }
    const auto second = jobs.submit("test.write", {}, context, "two");
    EXPECT_TRUE(second);
    const auto refused = jobs.submit("test.write", {}, context, "three");
    EXPECT_FALSE(refused);
    EXPECT_EQ(gate.calls, 3U);
    EXPECT_EQ(gate.releases, 1U);
    {
        const std::scoped_lock lock{mutex};
        release = true;
    }
    condition.notify_all();
    jobs.shutdown();
    EXPECT_EQ(gate.releases, 3U);
}
} // namespace
