#include "axklib/application/jobs.hpp"
#include "jobs_test_access.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;
using namespace axk::app;

struct AdmissionGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool released{};
    std::atomic_uint calls{};
    std::atomic_uint cleanup{};
    bool fail{};
    bool throws{};

    Result<std::shared_ptr<void>> admit() {
        ++calls;
        std::unique_lock lock{mutex};
        entered = true;
        condition.notify_all();
        if (!condition.wait_for(lock, 5s, [&] { return released; }))
            return std::unexpected(Error{"admission_timeout", "Test admission was not released"});
        if (throws)
            throw std::runtime_error{"Injected admission exception"};
        if (fail)
            return std::unexpected(Error{"admission_refused", "Injected refusal"});
        return std::shared_ptr<void>{new int{}, [this](void *value) {
                                         delete static_cast<int *>(value);
                                         ++cleanup;
                                     }};
    }
    bool wait() {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, 2s, [&] { return entered; });
    }
    void release() {
        const std::scoped_lock lock{mutex};
        released = true;
        condition.notify_all();
    }
};

OperationRegistry registry(AdmissionGate &gate, std::promise<void> *read_started = nullptr) {
    OperationRegistry result;
    EXPECT_TRUE(result.declare({"test.write",
                                "Write",
                                HttpMethod::post,
                                "/api/v1/test/write",
                                ExecutionMode::job,
                                {},
                                "Request",
                                "Result",
                                OperationClass::write,
                                true}));
    EXPECT_TRUE(result.declare({"test.read",
                                "Read",
                                HttpMethod::post,
                                "/api/v1/test/read",
                                ExecutionMode::job,
                                {},
                                "Request",
                                "Result",
                                OperationClass::read,
                                false}));
    for (const auto *id : {"test.write", "test.read"})
        EXPECT_TRUE(result.bind(id, [read_started, operation = std::string{id}](const auto &, const auto &) {
            if (read_started && operation == "test.read")
                read_started->set_value();
            return Result<nlohmann::json>{{{"ok", true}}};
        }));
    EXPECT_TRUE(result.bind_job_admission("test.write", [&](const auto &, const auto &) { return gate.admit(); }));
    return result;
}

OperationContext owner() {
    OperationContext context{};
    context.owner_id = "owner";
    return context;
}

TEST(JobSubmission, SlowAdmissionDoesNotBlockAnUnrelatedReadEvenWithOneQueueSlot) {
    AdmissionGate gate;
    std::promise<void> read_started;
    auto ran = read_started.get_future();
    auto operations = registry(gate, &read_started);
    JobManager jobs{operations, 1U, 1U, 1U, 8U};
    auto write = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "operation"); });
    const auto entered = gate.wait();
    auto read = std::async(std::launch::async, [&] { return jobs.submit("test.read", {}, owner()); });
    const auto ready = read.wait_for(200ms);
    if (ready == std::future_status::ready)
        EXPECT_EQ(ran.wait_for(2s), std::future_status::ready);
    gate.release();
    EXPECT_TRUE(entered);
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_TRUE(read.get());
    EXPECT_TRUE(write.get());
    jobs.shutdown();
}

TEST(JobSubmission, PendingKeyConflictsImmediatelyAndIdenticalFollowersShareOneAdmission) {
    AdmissionGate gate;
    auto operations = registry(gate);
    JobManager jobs{operations, 1U, 1U, 4U, 8U};
    auto first =
        std::async(std::launch::async, [&] { return jobs.submit("test.write", {{"value", 1}}, owner(), "key"); });
    const auto entered = gate.wait();
    auto follower =
        std::async(std::launch::async, [&] { return jobs.submit("test.write", {{"value", 1}}, owner(), "key"); });
    auto conflict =
        std::async(std::launch::async, [&] { return jobs.submit("test.write", {{"value", 2}}, owner(), "key"); });
    const auto ready = conflict.wait_for(200ms);
    gate.release();
    EXPECT_TRUE(entered);
    EXPECT_EQ(ready, std::future_status::ready);
    const auto refused = conflict.get();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "idempotency_conflict");
    const auto accepted = first.get();
    const auto replayed = follower.get();
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(replayed);
    EXPECT_EQ(accepted->job_id, replayed->job_id);
    EXPECT_EQ(gate.calls, 1U);
    jobs.shutdown();
    EXPECT_EQ(gate.cleanup, 1U);
}

TEST(JobSubmission, PendingLeadersConsumeRetainedCapacityButFollowersDoNot) {
    AdmissionGate gate;
    auto operations = registry(gate);
    JobManager jobs{operations, 1U, 1U, 4U, 8U, 1U};
    auto first = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "key"); });
    EXPECT_TRUE(gate.wait());
    auto follower = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "key"); });
    EXPECT_TRUE(JobManagerTestAccess::wait_for_submission_waiters(jobs, 1U, 2s));
    const auto refused = jobs.submit("test.read", {}, owner());
    gate.release();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "job_capacity_full");
    const auto accepted = first.get();
    const auto replayed = follower.get();
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(replayed);
    EXPECT_EQ(accepted->job_id, replayed->job_id);
    EXPECT_EQ(gate.calls, 1U);
}

TEST(JobSubmission, AdmissionFailuresAndExceptionsReachFollowersAndPermitLaterRetry) {
    for (const auto throws : {false, true}) {
        AdmissionGate gate;
        gate.fail = !throws;
        gate.throws = throws;
        auto operations = registry(gate);
        JobManager jobs{operations, 1U, 1U, 4U, 8U};
        auto first = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "key"); });
        EXPECT_TRUE(gate.wait());
        auto follower = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "key"); });
        EXPECT_TRUE(JobManagerTestAccess::wait_for_submission_waiters(jobs, 1U, 2s));
        gate.release();
        const auto failed = first.get();
        const auto replay = follower.get();
        ASSERT_FALSE(failed);
        ASSERT_FALSE(replay);
        EXPECT_EQ(failed.error().code, throws ? "job_submission_failed" : "admission_refused");
        EXPECT_EQ(failed.error().code, replay.error().code);
        EXPECT_EQ(gate.calls, 1U);
        gate.fail = false;
        gate.throws = false;
        EXPECT_TRUE(jobs.submit("test.write", {}, owner(), "key"));
        EXPECT_EQ(gate.calls, 2U);
    }
}

TEST(JobSubmission, FailedPublicationRollsBackBeforeReleasingTheAdmissionAndCanRetry) {
    AdmissionGate gate;
    gate.release();
    auto operations = registry(gate);
    JobManager jobs{operations, 1U, 1U, 4U, 8U, 1U};
    JobManagerTestAccess::set_before_queue_publication(jobs, [] { throw std::runtime_error{"publication failure"}; });
    const auto refused = jobs.submit("test.write", {}, owner(), "key");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "job_submission_failed");
    EXPECT_EQ(gate.cleanup, 1U);
    EXPECT_EQ(jobs.metrics().submitted_jobs, 0U);
    JobManagerTestAccess::set_before_queue_publication(jobs, {});
    EXPECT_TRUE(jobs.submit("test.write", {}, owner(), "key"));
    EXPECT_EQ(gate.calls, 2U);
}

TEST(JobSubmission, ShutdownDrainsPendingLeadersAndFollowersWithoutPublishingAJob) {
    AdmissionGate gate;
    auto operations = registry(gate);
    JobManager jobs{operations, 1U, 1U, 4U, 8U};
    auto first = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "key"); });
    EXPECT_TRUE(gate.wait());
    auto follower = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "key"); });
    EXPECT_TRUE(JobManagerTestAccess::wait_for_submission_waiters(jobs, 1U, 2s));
    auto shutdown = std::async(std::launch::async, [&] { jobs.shutdown(); });
    EXPECT_TRUE(JobManagerTestAccess::wait_until_stopping(jobs, 2s));
    EXPECT_EQ(shutdown.wait_for(40ms), std::future_status::timeout);
    gate.release();
    const auto refused = first.get();
    const auto replay = follower.get();
    shutdown.get();
    ASSERT_FALSE(refused);
    ASSERT_FALSE(replay);
    EXPECT_EQ(refused.error().code, "job_runtime_stopping");
    EXPECT_EQ(refused.error().code, replay.error().code);
    EXPECT_EQ(gate.cleanup, 1U);
    EXPECT_EQ(jobs.metrics().submitted_jobs, 0U);
}

TEST(JobSubmission, QueuedSubscriberCanReplayItsKeyAndShutdownReentrantly) {
    AdmissionGate gate;
    gate.release();
    auto operations = registry(gate);
    JobManager jobs{operations, 1U, 1U, 4U, 8U};
    std::optional<JobSnapshot> nested;
    const auto subscription = jobs.subscribe([&](const JobEvent &event) {
        if (event.type != "queued")
            return;
        const auto replay = jobs.submit("test.write", {}, owner(), "key");
        EXPECT_TRUE(replay);
        if (replay)
            nested = *replay;
        jobs.shutdown();
    });
    const auto outer = jobs.submit("test.write", {}, owner(), "key");
    ASSERT_TRUE(outer);
    ASSERT_TRUE(nested);
    EXPECT_EQ(outer->job_id, nested->job_id);
    EXPECT_EQ(gate.calls, 1U);
    jobs.unsubscribe(subscription);
}

TEST(JobSubmission, ConcurrentShutdownAndQueuedNotificationRetainTheRuntime) {
    AdmissionGate gate;
    gate.release();
    auto operations = registry(gate);
    JobManager jobs{operations, 1U, 1U, 4U, 8U};
    std::promise<void> notifying;
    std::promise<void> release;
    const auto released = release.get_future().share();
    const auto subscription = jobs.subscribe([&](const JobEvent &event) {
        if (event.type != "queued")
            return;
        notifying.set_value();
        released.wait();
        jobs.shutdown();
    });
    auto submitted = std::async(std::launch::async, [&] { return jobs.submit("test.write", {}, owner(), "key"); });
    const auto entered = notifying.get_future().wait_for(2s);
    auto stopped = std::async(std::launch::async, [&] { jobs.shutdown(); });
    const auto drained = stopped.wait_for(2s);
    release.set_value();
    EXPECT_EQ(entered, std::future_status::ready);
    EXPECT_EQ(drained, std::future_status::ready);
    stopped.get();
    EXPECT_TRUE(submitted.get());
    jobs.unsubscribe(subscription);
}

} // namespace
