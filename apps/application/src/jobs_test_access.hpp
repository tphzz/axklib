#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string_view>

namespace axk::app {

class JobManager;

class JobManagerTestAccess {
  public:
    static void set_before_running_claim(JobManager &manager, std::function<void()> hook);
    static void set_before_queue_publication(JobManager &manager, std::function<void()> hook);
    static bool wait_for_submission_waiters(JobManager &manager, std::size_t count, std::chrono::milliseconds timeout);
    static bool wait_until_stopping(JobManager &manager, std::chrono::milliseconds timeout);
    static void set_before_terminal_cleanup(JobManager &manager, std::function<void(std::string_view)> hook);
};

} // namespace axk::app
