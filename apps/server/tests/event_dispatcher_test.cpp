#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include "axklib/server/event_dispatcher.hpp"

namespace {

using namespace std::chrono_literals;

axk::app::JobEvent event(std::uint64_t sequence, std::string type = "progress",
                         axk::app::JobState state = axk::app::JobState::running) {
    return {.event_id = "event-" + std::to_string(sequence),
            .sequence = sequence,
            .job_id = "job",
            .operation_id = "operation",
            .owner_id = "owner",
            .type = std::move(type),
            .state = state,
            .timestamp_unix_ms = sequence,
            .progress = std::nullopt};
}

TEST(EventDispatcher, SlowSinkNeverBlocksPublishAndQueueRemainsBounded) {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool release{};
    axk::server::EventDispatcher dispatcher{1U, [&](const axk::app::JobEvent &) {
                                                std::unique_lock lock{mutex};
                                                entered = true;
                                                condition.notify_all();
                                                condition.wait(lock, [&] { return release; });
                                            }};
    EXPECT_TRUE(dispatcher.publish(event(1U)));
    {
        std::unique_lock lock{mutex};
        ASSERT_TRUE(condition.wait_for(lock, 1s, [&] { return entered; }));
    }
    EXPECT_TRUE(dispatcher.publish(event(2U)));
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(dispatcher.publish(event(3U)));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
    const auto blocked = dispatcher.snapshot();
    EXPECT_EQ(blocked.pending_events, 1U);
    EXPECT_EQ(blocked.dropped_events, 1U);
    {
        const std::scoped_lock lock{mutex};
        release = true;
    }
    condition.notify_all();
    dispatcher.shutdown();
    const auto complete = dispatcher.snapshot();
    EXPECT_EQ(complete.delivered_events, 2U);
    EXPECT_EQ(complete.pending_events, 0U);
}

TEST(EventDispatcher, TerminalEventCanReplaceQueuedProgress) {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool release{};
    std::vector<std::string> delivered;
    axk::server::EventDispatcher dispatcher{1U, [&](const axk::app::JobEvent &current) {
                                                {
                                                    const std::scoped_lock lock{mutex};
                                                    delivered.push_back(current.type);
                                                    entered = true;
                                                }
                                                condition.notify_all();
                                                std::unique_lock lock{mutex};
                                                condition.wait(lock, [&] { return release; });
                                            }};
    EXPECT_TRUE(dispatcher.publish(event(1U, "running")));
    {
        std::unique_lock lock{mutex};
        ASSERT_TRUE(condition.wait_for(lock, 1s, [&] { return entered; }));
    }
    EXPECT_TRUE(dispatcher.publish(event(2U)));
    EXPECT_TRUE(dispatcher.publish(event(3U, "completed", axk::app::JobState::completed)));
    {
        const std::scoped_lock lock{mutex};
        release = true;
    }
    condition.notify_all();
    dispatcher.shutdown();
    EXPECT_EQ(delivered, (std::vector<std::string>{"running", "completed"}));
    EXPECT_EQ(dispatcher.snapshot().dropped_events, 1U);
}

} // namespace
