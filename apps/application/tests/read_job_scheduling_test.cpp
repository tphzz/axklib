#include "axklib/application/jobs.hpp"
#include "jobs_test_access.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;
using namespace axk::app;

class ReadJobs : public testing::Test {
  protected:
    OperationRegistry registry;
    std::mutex mutex;
    std::condition_variable changed;
    bool started{};
    bool observed_cancellation{};
    bool release{};
    std::atomic_uint calls{};

    void SetUp() override {
        ASSERT_TRUE(registry.declare({"test.read",
                                      "test read",
                                      HttpMethod::post,
                                      "/api/v1/test-read",
                                      ExecutionMode::job,
                                      {},
                                      "Request",
                                      "Result"}));
        ASSERT_TRUE(registry.bind("test.read",
                                  [this](const auto &input, const OperationContext &context) -> Result<nlohmann::json> {
                                      ++calls;
                                      if (!input.value("hold", false))
                                          return nlohmann::json{{"ok", true}};
                                      std::unique_lock lock{mutex};
                                      started = true;
                                      changed.notify_all();
                                      const auto deadline = std::chrono::steady_clock::now() + 5s;
                                      while (!release && std::chrono::steady_clock::now() < deadline) {
                                          if (context.cancellation.is_cancelled()) {
                                              observed_cancellation = true;
                                              changed.notify_all();
                                          }
                                          changed.wait_for(lock, 1ms);
                                      }
                                      if (context.cancellation.is_cancelled())
                                          return std::unexpected(Error{"operation_cancelled", "cancelled"});
                                      return nlohmann::json{{"ok", true}};
                                  }));
        ASSERT_TRUE(registry.bind_read_scheduling(
            "test.read", [](const auto &input, const OperationContext &) -> Result<std::optional<ReadJobScheduling>> {
                if (input.value("invalid", false))
                    return std::unexpected(Error{"invalid_request", "invalid target"});
                return ReadJobScheduling{input.value("scope", "image:1"),
                                         input.value("play", false) ? ReadIntent::playback : ReadIntent::selected};
            }));
    }

    bool wait_flag(bool &flag) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, 2s, [&] { return flag; });
    }
    void unblock() {
        const std::scoped_lock lock{mutex};
        release = true;
        changed.notify_all();
    }
    static OperationContext owner(std::string name = "owner") {
        OperationContext context;
        context.owner_id = std::move(name);
        return context;
    }
    static JobSnapshot terminal(JobManager &jobs, const std::string &id) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status = jobs.status(id, "owner");
            if (status && is_terminal(status->state))
                return *status;
            std::this_thread::sleep_for(1ms);
        }
        ADD_FAILURE() << "read job did not finish";
        return {};
    }
};

TEST_F(ReadJobs, PlaybackWaitsForTheCancelledHandlerToActuallyReleaseItsGroup) {
    JobManager jobs{registry, 2U, 1U, 8U, 16U};
    const auto low = jobs.submit("test.read", {{"hold", true}}, owner());
    ASSERT_TRUE(low);
    ASSERT_TRUE(wait_flag(started));
    const auto high = jobs.submit("test.read", {{"play", true}}, owner());
    EXPECT_TRUE(high);
    EXPECT_TRUE(wait_flag(observed_cancellation));
    EXPECT_EQ(calls.load(), 1U);
    if (high)
        EXPECT_EQ(jobs.status(high->job_id, "owner")->state, JobState::queued);
    unblock();
    const auto interrupted = terminal(jobs, low->job_id);
    EXPECT_EQ(interrupted.state, JobState::cancelled);
    ASSERT_TRUE(interrupted.error);
    EXPECT_EQ(interrupted.error->code, "read_preempted");
    if (high)
        EXPECT_EQ(terminal(jobs, high->job_id).state, JobState::completed);
}

TEST_F(ReadJobs, PlaybackReplacesQueuedLowerPriorityWorkWithoutRaisingQueueCapacity) {
    JobManager jobs{registry, 1U, 1U, 1U, 16U};
    const auto active = jobs.submit("test.read", {{"hold", true}}, owner());
    ASSERT_TRUE(active);
    ASSERT_TRUE(wait_flag(started));
    const auto queued = jobs.submit("test.read", nlohmann::json::object(), owner());
    EXPECT_TRUE(queued);
    const auto high = jobs.submit("test.read", {{"play", true}}, owner());
    EXPECT_TRUE(high);
    unblock();
    if (high)
        EXPECT_EQ(terminal(jobs, high->job_id).state, JobState::completed);
    if (queued)
        EXPECT_EQ(terminal(jobs, queued->job_id).state, JobState::cancelled);
    EXPECT_EQ(calls.load(), 2U);
}

TEST_F(ReadJobs, InvalidRequestsAndOtherOwnersCannotPreemptAnActiveRead) {
    JobManager jobs{registry, 2U, 1U, 8U, 16U};
    const auto active = jobs.submit("test.read", {{"hold", true}}, owner());
    ASSERT_TRUE(active);
    ASSERT_TRUE(wait_flag(started));
    EXPECT_FALSE(jobs.submit("test.read", {{"invalid", true}, {"play", true}}, owner()));
    const auto other = jobs.submit("test.read", {{"play", true}}, owner("other"));
    EXPECT_TRUE(other);
    const auto state = jobs.status(active->job_id, "owner");
    EXPECT_EQ(state->state, JobState::running);
    {
        const std::scoped_lock lock{mutex};
        EXPECT_FALSE(observed_cancellation);
    }
    unblock();
    EXPECT_EQ(terminal(jobs, active->job_id).state, JobState::completed);
}

TEST_F(ReadJobs, DifferentImageOrRevisionScopesDoNotPreempt) {
    JobManager jobs{registry, 2U, 1U, 8U, 16U};
    const auto active = jobs.submit("test.read", {{"hold", true}, {"scope", "image:1"}}, owner());
    ASSERT_TRUE(active);
    ASSERT_TRUE(wait_flag(started));
    for (const auto scope : {"image:2", "other:1"}) {
        const auto other = jobs.submit("test.read", {{"play", true}, {"scope", scope}}, owner());
        EXPECT_TRUE(other);
        if (other)
            EXPECT_EQ(terminal(jobs, other->job_id).state, JobState::completed);
    }
    {
        const std::scoped_lock lock{mutex};
        EXPECT_FALSE(observed_cancellation);
    }
    unblock();
    EXPECT_EQ(terminal(jobs, active->job_id).state, JobState::completed);
}

TEST_F(ReadJobs, RejectedRetainedCapacityDoesNotCancelExistingWork) {
    JobManager jobs{registry, 2U, 1U, 8U, 16U, 1U};
    const auto active = jobs.submit("test.read", {{"hold", true}}, owner());
    ASSERT_TRUE(active);
    ASSERT_TRUE(wait_flag(started));
    const auto high = jobs.submit("test.read", {{"play", true}}, owner());
    ASSERT_FALSE(high);
    EXPECT_EQ(high.error().code, "job_capacity_full");
    unblock();
    EXPECT_EQ(terminal(jobs, active->job_id).state, JobState::completed);
}

TEST_F(ReadJobs, DequeuedReadsReserveTheirGroupBeforeClaimAndDoNotDiscountUnrelatedQueuedWork) {
    JobManager jobs{registry, 2U, 1U, 1U, 16U};
    bool claimed{};
    bool resume{};
    JobManagerTestAccess::set_before_running_claim(jobs, [&] {
        std::unique_lock lock{mutex};
        claimed = true;
        changed.notify_all();
        changed.wait_for(lock, 3s, [&] { return resume; });
    });
    const auto dequeued = jobs.submit("test.read", nlohmann::json::object(), owner());
    ASSERT_TRUE(dequeued);
    ASSERT_TRUE(wait_flag(claimed));
    const auto queued = jobs.submit("test.read", nlohmann::json::object(), owner());
    EXPECT_TRUE(queued);
    // Both lower-priority records can be preempted, but only the one still in
    // the queue frees capacity. A reserved group must not run concurrently.
    const auto high = jobs.submit("test.read", {{"play", true}}, owner());
    EXPECT_TRUE(high);
    EXPECT_EQ(calls.load(), 0U);
    if (high)
        EXPECT_EQ(jobs.status(high->job_id, "owner")->state, JobState::queued);
    JobManagerTestAccess::set_before_running_claim(jobs, {});
    {
        const std::scoped_lock lock{mutex};
        resume = true;
        changed.notify_all();
    }
    EXPECT_EQ(terminal(jobs, dequeued->job_id).state, JobState::cancelled);
    if (queued)
        EXPECT_EQ(terminal(jobs, queued->job_id).state, JobState::cancelled);
    if (high)
        EXPECT_EQ(terminal(jobs, high->job_id).state, JobState::completed);
    EXPECT_EQ(calls.load(), 1U);
}

TEST_F(ReadJobs, WriteOperationsCannotRegisterReadScheduling) {
    ASSERT_TRUE(registry.declare({"test.write",
                                  "write",
                                  HttpMethod::post,
                                  "/api/v1/test-write",
                                  ExecutionMode::job,
                                  {},
                                  "Request",
                                  "Result",
                                  OperationClass::write}));
    EXPECT_FALSE(registry.bind_read_scheduling(
        "test.write", [](const auto &, const OperationContext &) -> Result<std::optional<ReadJobScheduling>> {
            return ReadJobScheduling{"image:1", ReadIntent::playback};
        }));
}
} // namespace
