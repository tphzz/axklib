#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "axklib/application/jobs.hpp"

namespace {

using namespace std::chrono_literals;

axk::app::OperationRegistry test_registry(
    std::function<axk::app::Result<nlohmann::json>(const nlohmann::json &, const axk::app::OperationContext &)> handler,
    axk::app::OperationClass operation_class = axk::app::OperationClass::read, bool requires_idempotency = false) {
    axk::app::OperationRegistry registry;
    EXPECT_TRUE(registry.declare({"test.job",
                                  "axklib test job",
                                  axk::app::HttpMethod::post,
                                  "/api/v1/test-jobs",
                                  axk::app::ExecutionMode::job,
                                  {},
                                  "TestRequest",
                                  "TestResult",
                                  operation_class,
                                  requires_idempotency}));
    EXPECT_TRUE(registry.bind("test.job", std::move(handler)));
    return registry;
}

axk::app::JobSnapshot wait_terminal(axk::app::JobManager &jobs, std::string_view job_id,
                                    std::string_view owner = "owner") {
    for (std::size_t attempt = 0; attempt < 2000U; ++attempt) {
        auto status = jobs.status(job_id, owner);
        EXPECT_TRUE(status) << status.error().message;
        if (status && axk::app::is_terminal(status->state))
            return *status;
        std::this_thread::sleep_for(1ms);
    }
    ADD_FAILURE() << "job did not reach a terminal state";
    return {};
}

TEST(JobManager, CompletesOperationsAndRetainsOrderedReplay) {
    auto registry = test_registry([](const nlohmann::json &request, const axk::app::OperationContext &context) {
        if (context.progress != nullptr)
            context.progress->report({axk::ProgressPhase::reading, 1U, 2U, "reading", std::nullopt});
        return axk::app::Result<nlohmann::json>{nlohmann::json{{"echo", request.at("value")}}};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 8U};
    std::mutex event_mutex;
    std::vector<axk::app::JobEvent> observed;
    const auto subscription = jobs.subscribe([&](const axk::app::JobEvent &event) {
        const std::scoped_lock lock{event_mutex};
        observed.push_back(event);
    });

    auto submitted = jobs.submit(
        "test.job", {{"value", 7}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(submitted) << submitted.error().message;
    EXPECT_EQ(submitted->state, axk::app::JobState::queued);
    EXPECT_EQ(submitted->job_id.size(), 36U);
    EXPECT_TRUE(submitted->job_id.starts_with("job-"));
    const auto terminal = wait_terminal(jobs, submitted->job_id);
    ASSERT_EQ(terminal.state, axk::app::JobState::completed);
    ASSERT_TRUE(terminal.result);
    EXPECT_EQ(terminal.result->at("echo"), 7);

    const auto replay = jobs.replay(submitted->job_id, "owner", 0U);
    ASSERT_TRUE(replay) << replay.error().message;
    ASSERT_GE(replay->size(), 3U);
    for (std::size_t index = 1; index < replay->size(); ++index)
        EXPECT_EQ((*replay)[index].sequence, (*replay)[index - 1U].sequence + 1U);
    EXPECT_EQ(replay->front().state, axk::app::JobState::queued);
    EXPECT_EQ(replay->back().state, axk::app::JobState::completed);
    jobs.unsubscribe(subscription);
}

TEST(JobManager, CancelsQueuedWorkAndEnforcesOwnership) {
    std::mutex mutex;
    std::condition_variable condition;
    bool release{};
    std::atomic_uint32_t calls{};
    auto registry = test_registry([&](const nlohmann::json &, const axk::app::OperationContext &context) {
        calls.fetch_add(1U);
        std::unique_lock lock{mutex};
        condition.wait(lock, [&] { return release || context.cancellation.is_cancelled(); });
        if (auto active = context.cancellation.check(); !active)
            return axk::app::Result<nlohmann::json>{
                std::unexpected(axk::app::Error{"cancelled", "operation was cancelled"})};
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 2U, 8U};
    auto first = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(first);
    while (calls.load() == 0U)
        std::this_thread::sleep_for(1ms);
    auto second = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "two", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(second);

    const auto hidden = jobs.status(second->job_id, "different-owner");
    ASSERT_FALSE(hidden);
    EXPECT_EQ(hidden.error().code, "job_not_found");
    EXPECT_TRUE(jobs.cancel(second->job_id, "owner"));
    EXPECT_TRUE(jobs.cancel(second->job_id, "owner"));
    EXPECT_EQ(wait_terminal(jobs, second->job_id).state, axk::app::JobState::cancelled);
    EXPECT_EQ(calls.load(), 1U);

    {
        const std::scoped_lock lock{mutex};
        release = true;
    }
    condition.notify_all();
    EXPECT_EQ(wait_terminal(jobs, first->job_id).state, axk::app::JobState::completed);
}

TEST(JobManager, TracksWorkspaceReferencesOnlyWhileJobsAreActive) {
    std::atomic_bool release{};
    auto registry = test_registry([&](const nlohmann::json &, const axk::app::OperationContext &context) {
        while (!release.load() && !context.cancellation.is_cancelled())
            std::this_thread::sleep_for(1ms);
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 2U, 8U};
    auto submitted = jobs.submit(
        "test.job", {{"source", {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(submitted) << submitted.error().message;
    EXPECT_TRUE(jobs.root_in_use("workspace"));
    EXPECT_TRUE(jobs.path_in_use({"workspace", "fixture.hds"}));
    EXPECT_TRUE(jobs.path_in_use({"workspace", ""}));
    EXPECT_FALSE(jobs.path_in_use({"workspace", "other.hds"}));
    EXPECT_FALSE(jobs.root_in_use("other"));
    release.store(true);
    EXPECT_EQ(wait_terminal(jobs, submitted->job_id).state, axk::app::JobState::completed);
    EXPECT_FALSE(jobs.root_in_use("workspace"));
    EXPECT_FALSE(jobs.path_in_use({"workspace", "fixture.hds"}));
}

TEST(JobManager, RejectsQueueOverflowAndNonJobOperations) {
    std::atomic_bool release{};
    auto registry = test_registry([&](const nlohmann::json &, const axk::app::OperationContext &context) {
        while (!release.load() && !context.cancellation.is_cancelled())
            std::this_thread::sleep_for(1ms);
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 1U, 4U};
    auto first = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(first);
    for (;;) {
        auto state = jobs.status(first->job_id, "owner");
        ASSERT_TRUE(state);
        if (state->state == axk::app::JobState::running)
            break;
        std::this_thread::sleep_for(1ms);
    }
    auto second = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "two", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(second);
    const auto overflow = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "three", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, "job_queue_full");
    EXPECT_TRUE(overflow.error().retryable);

    const auto wrong_mode = jobs.submit(
        "missing", {},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_FALSE(wrong_mode);
    EXPECT_EQ(wrong_mode.error().code, "unknown_operation");
    release = true;
}

TEST(JobManager, RequiresAndReplaysIdempotentWriteSubmission) {
    std::atomic_uint32_t calls{};
    auto registry = test_registry(
        [&](const nlohmann::json &request, const axk::app::OperationContext &) {
            calls.fetch_add(1U);
            return axk::app::Result<nlohmann::json>{request};
        },
        axk::app::OperationClass::write, true);
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 8U};
    const auto context = axk::app::OperationContext{
        .owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto missing = jobs.submit("test.job", {{"value", 1}}, context);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "idempotency_key_required");

    const auto first = jobs.submit("test.job", {{"value", 1}}, context, "write-one");
    ASSERT_TRUE(first) << first.error().message;
    const auto replay = jobs.submit("test.job", {{"value", 1}}, context, "write-one");
    ASSERT_TRUE(replay) << replay.error().message;
    EXPECT_EQ(replay->job_id, first->job_id);
    EXPECT_EQ(wait_terminal(jobs, first->job_id).state, axk::app::JobState::completed);
    EXPECT_EQ(calls.load(), 1U);

    const auto conflict = jobs.submit("test.job", {{"value", 2}}, context, "write-one");
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, "idempotency_conflict");
}

TEST(JobManager, ExpiresTerminalJobsAndBoundsRetainedRecords) {
    auto now = axk::app::JobManager::Clock::time_point{};
    auto registry = test_registry([](const nlohmann::json &, const axk::app::OperationContext &) {
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 2U, 4U, 1U, 5s, [&] { return now; }};
    const auto context = axk::app::OperationContext{
        .owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto first = jobs.submit("test.job", {}, context);
    ASSERT_TRUE(first);
    EXPECT_EQ(wait_terminal(jobs, first->job_id).state, axk::app::JobState::completed);

    const auto full = jobs.submit("test.job", {}, context);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, "job_capacity_full");
    EXPECT_TRUE(full.error().retryable);
    now += 5s;
    const auto expired = jobs.status(first->job_id, "owner");
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "job_not_found");
    EXPECT_TRUE(jobs.submit("test.job", {}, context));
}

TEST(JobManager, RunsReadAndWriteClassesIndependently) {
    std::atomic_bool release_read{};
    axk::app::OperationRegistry registry;
    ASSERT_TRUE(registry.declare({"test.read",
                                  "axklib test read",
                                  axk::app::HttpMethod::post,
                                  "/api/v1/test-read",
                                  axk::app::ExecutionMode::job,
                                  {},
                                  "TestRequest",
                                  "TestResult",
                                  axk::app::OperationClass::read,
                                  false}));
    ASSERT_TRUE(registry.declare({"test.write",
                                  "axklib test write",
                                  axk::app::HttpMethod::post,
                                  "/api/v1/test-write",
                                  axk::app::ExecutionMode::job,
                                  {},
                                  "TestRequest",
                                  "TestResult",
                                  axk::app::OperationClass::write,
                                  true}));
    ASSERT_TRUE(registry.bind("test.read", [&](const nlohmann::json &, const axk::app::OperationContext &context) {
        while (!release_read.load() && !context.cancellation.is_cancelled())
            std::this_thread::sleep_for(1ms);
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    }));
    ASSERT_TRUE(registry.bind("test.write", [](const nlohmann::json &, const axk::app::OperationContext &) {
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    }));
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 4U};
    const auto context = axk::app::OperationContext{
        .owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto read = jobs.submit("test.read", {}, context);
    ASSERT_TRUE(read);
    for (;;) {
        const auto status = jobs.status(read->job_id, "owner");
        ASSERT_TRUE(status);
        if (status->state == axk::app::JobState::running)
            break;
        std::this_thread::sleep_for(1ms);
    }
    const auto write = jobs.submit("test.write", {}, context, "write-one");
    ASSERT_TRUE(write);
    EXPECT_EQ(wait_terminal(jobs, write->job_id).state, axk::app::JobState::completed);
    release_read = true;
    EXPECT_EQ(wait_terminal(jobs, read->job_id).state, axk::app::JobState::completed);
}

TEST(JobManager, ReservesConflictingWriteDestinationsUntilTheActiveJobTerminates) {
    std::atomic_bool release{};
    auto registry = test_registry(
        [&](const nlohmann::json &, const axk::app::OperationContext &context) {
            while (!release.load() && !context.cancellation.is_cancelled())
                std::this_thread::sleep_for(1ms);
            return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
        },
        axk::app::OperationClass::write, true);
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 8U};
    const auto context = axk::app::OperationContext{
        .owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const nlohmann::json request{{"destination", {{"rootId", "workspace"}, {"relativePath", "exports/result"}}}};
    const auto first = jobs.submit("test.job", request, context, "write-one");
    ASSERT_TRUE(first) << first.error().message;
    for (;;) {
        const auto state = jobs.status(first->job_id, "owner");
        ASSERT_TRUE(state);
        if (state->state == axk::app::JobState::running)
            break;
        std::this_thread::sleep_for(1ms);
    }

    const auto same = jobs.submit("test.job", request, context, "write-two");
    ASSERT_FALSE(same);
    EXPECT_EQ(same.error().code, "destination_reserved");
    EXPECT_TRUE(same.error().retryable);
    const auto child = jobs.submit(
        "test.job", {{"output", {{"rootId", "workspace"}, {"relativePath", "exports/result/child.axkpkg"}}}}, context,
        "write-three");
    ASSERT_FALSE(child);
    EXPECT_EQ(child.error().code, "destination_reserved");

    release = true;
    EXPECT_EQ(wait_terminal(jobs, first->job_id).state, axk::app::JobState::completed);
    const auto after = jobs.submit("test.job", request, context, "write-four");
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(wait_terminal(jobs, after->job_id).state, axk::app::JobState::completed);
}

TEST(JobManager, RejectsExpiredReplayAndKeepsTerminalStateImmutable) {
    auto registry = test_registry([](const nlohmann::json &, const axk::app::OperationContext &context) {
        for (std::uint64_t completed = 1U; completed <= 4U; ++completed) {
            if (context.progress != nullptr)
                context.progress->report({axk::ProgressPhase::reading, completed, 4U, "reading", std::nullopt});
        }
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 2U};
    const auto submitted = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(submitted);
    const auto terminal = wait_terminal(jobs, submitted->job_id);
    ASSERT_EQ(terminal.state, axk::app::JobState::completed);
    const auto expired = jobs.replay(submitted->job_id, "owner", 0U);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "job_event_replay_expired");

    ASSERT_TRUE(jobs.cancel(submitted->job_id, "owner"));
    const auto unchanged = jobs.status(submitted->job_id, "owner");
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->state, axk::app::JobState::completed);
    EXPECT_EQ(unchanged->latest_sequence, terminal.latest_sequence);
}

TEST(JobManager, IgnoresRegressingProgressWithinAPhaseAndContainsSubscriberFailures) {
    auto registry = test_registry([](const nlohmann::json &, const axk::app::OperationContext &context) {
        if (context.progress == nullptr)
            return axk::app::Result<nlohmann::json>{
                std::unexpected(axk::app::Error{"missing_progress", "job progress adapter is missing"})};
        context.progress->report({axk::ProgressPhase::reading, 2U, 4U, "two", std::nullopt});
        context.progress->report({axk::ProgressPhase::reading, 1U, 4U, "one", std::nullopt});
        context.progress->report({axk::ProgressPhase::reading, 3U, 4U, "three", std::nullopt});
        context.progress->report({axk::ProgressPhase::writing, 0U, 1U, "writing", std::nullopt});
        return axk::app::Result<nlohmann::json>{nlohmann::json::object()};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 16U};
    const auto throwing = jobs.subscribe([](const axk::app::JobEvent &) { throw std::runtime_error{"sink"}; });
    ASSERT_NE(throwing, 0U);
    const auto submitted = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(submitted);
    EXPECT_EQ(wait_terminal(jobs, submitted->job_id).state, axk::app::JobState::completed);
    const auto replay = jobs.replay(submitted->job_id, "owner", 0U);
    ASSERT_TRUE(replay);
    std::vector<std::uint64_t> reading;
    for (const auto &event : *replay) {
        if (event.type == "progress" && event.progress && event.progress->phase == "reading")
            reading.push_back(event.progress->completed);
    }
    EXPECT_EQ(reading, (std::vector<std::uint64_t>{2U, 3U}));
    jobs.unsubscribe(throwing);
}

TEST(JobManager, ConcurrentCancellationOfRunningWorkPublishesOneImmutableTerminalState) {
    std::atomic_bool entered{};
    auto registry = test_registry([&](const nlohmann::json &, const axk::app::OperationContext &context) {
        entered = true;
        while (!context.cancellation.is_cancelled())
            std::this_thread::sleep_for(1ms);
        return axk::app::Result<nlohmann::json>{
            std::unexpected(axk::app::Error{"cancelled", "operation was cancelled"})};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 16U};
    const auto submitted = jobs.submit(
        "test.job", {},
        {.owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(submitted);
    while (!entered.load())
        std::this_thread::sleep_for(1ms);

    std::vector<std::jthread> callers;
    for (std::size_t index = 0; index < 16U; ++index) {
        callers.emplace_back([&] { EXPECT_TRUE(jobs.cancel(submitted->job_id, "owner")); });
    }
    callers.clear();
    const auto terminal = wait_terminal(jobs, submitted->job_id);
    ASSERT_EQ(terminal.state, axk::app::JobState::cancelled);
    const auto replay = jobs.replay(submitted->job_id, "owner", 0U);
    ASSERT_TRUE(replay);
    EXPECT_EQ(std::ranges::count(*replay, "cancellation_requested", &axk::app::JobEvent::type), 1);
    EXPECT_EQ(std::ranges::count(*replay, "cancelled", &axk::app::JobEvent::type), 1);
    EXPECT_TRUE(jobs.cancel(submitted->job_id, "owner"));
    const auto unchanged = jobs.status(submitted->job_id, "owner");
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->state, axk::app::JobState::cancelled);
    EXPECT_EQ(unchanged->latest_sequence, terminal.latest_sequence);

    const auto metrics = jobs.metrics();
    EXPECT_EQ(metrics.submitted_jobs, 1U);
    EXPECT_EQ(metrics.queued_jobs, 0U);
    EXPECT_EQ(metrics.running_jobs, 0U);
    EXPECT_EQ(metrics.cancelled_jobs, 1U);
    EXPECT_GE(metrics.published_events, 4U);
}

TEST(JobManager, ShutdownCancelsRunningWorkAndRejectsNewAdmission) {
    std::atomic_bool entered{};
    auto registry = test_registry([&](const nlohmann::json &, const axk::app::OperationContext &context) {
        entered = true;
        while (!context.cancellation.is_cancelled())
            std::this_thread::sleep_for(1ms);
        return axk::app::Result<nlohmann::json>{
            std::unexpected(axk::app::Error{"cancelled", "operation was cancelled"})};
    });
    axk::app::JobManager jobs{registry, 1U, 1U, 4U, 8U};
    const auto context = axk::app::OperationContext{
        .owner_id = "owner", .request_id = "one", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto submitted = jobs.submit("test.job", {}, context);
    ASSERT_TRUE(submitted);
    while (!entered.load())
        std::this_thread::sleep_for(1ms);
    jobs.shutdown();
    EXPECT_EQ(wait_terminal(jobs, submitted->job_id).state, axk::app::JobState::cancelled);
    const auto rejected = jobs.submit("test.job", {}, context);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "job_runtime_stopping");
    EXPECT_TRUE(rejected.error().retryable);
}

} // namespace
