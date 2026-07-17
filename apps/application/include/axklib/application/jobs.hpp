#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"

namespace axk::app {

enum class JobState : std::uint8_t { queued, running, completed, failed, cancelled };

struct JobProgress {
    std::string phase;
    std::uint64_t completed{};
    std::optional<std::uint64_t> total;
    std::string message;
};

struct JobEvent {
    std::string event_id;
    std::uint64_t sequence{};
    std::string job_id;
    std::string operation_id;
    std::string owner_id;
    std::string type;
    JobState state{JobState::queued};
    std::uint64_t timestamp_unix_ms{};
    std::optional<JobProgress> progress;
};

struct JobSnapshot {
    std::string job_id;
    std::string operation_id;
    JobState state{JobState::queued};
    std::uint64_t latest_sequence{};
    std::optional<JobProgress> progress;
    std::optional<nlohmann::json> result;
    std::optional<Error> error;
};

struct JobRuntimeMetrics {
    std::uint64_t submitted_jobs{};
    std::uint64_t queued_jobs{};
    std::uint64_t running_jobs{};
    std::uint64_t completed_jobs{};
    std::uint64_t failed_jobs{};
    std::uint64_t cancelled_jobs{};
    std::uint64_t published_events{};
    std::uint64_t progress_events{};
    std::uint64_t total_queue_wait_ms{};
    std::uint64_t total_execution_ms{};
    std::uint64_t total_phase_duration_ms{};
    std::uint64_t total_cancellation_latency_ms{};
};

class JobManager {
  public:
    using EventSink = std::function<void(const JobEvent &)>;
    using SubscriptionId = std::uint64_t;
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    JobManager(
        const OperationRegistry &registry, std::size_t read_worker_count, std::size_t write_worker_count,
        std::size_t maximum_queued_jobs, std::size_t replay_events_per_job, std::size_t maximum_retained_jobs = 2048U,
        std::chrono::seconds retention = std::chrono::minutes{15}, Now now = [] { return Clock::now(); });
    ~JobManager();

    JobManager(const JobManager &) = delete;
    JobManager &operator=(const JobManager &) = delete;
    JobManager(JobManager &&) = delete;
    JobManager &operator=(JobManager &&) = delete;

    [[nodiscard]] Result<JobSnapshot> submit(std::string operation_id, nlohmann::json request, OperationContext context,
                                             std::optional<std::string> idempotency_key = std::nullopt);
    [[nodiscard]] Result<JobSnapshot> status(std::string_view job_id, std::string_view owner_id) const;
    [[nodiscard]] Result<void> cancel(std::string_view job_id, std::string_view owner_id);
    [[nodiscard]] Result<std::vector<JobEvent>> replay(std::string_view job_id, std::string_view owner_id,
                                                       std::uint64_t after_sequence) const;
    [[nodiscard]] JobRuntimeMetrics metrics() const noexcept;
    [[nodiscard]] bool root_in_use(std::string_view root_id) const;
    [[nodiscard]] bool path_in_use(const FileRef &reference) const;

    [[nodiscard]] SubscriptionId subscribe(EventSink sink);
    void unsubscribe(SubscriptionId subscription_id) noexcept;
    void shutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view job_state_name(JobState state) noexcept;
[[nodiscard]] bool is_terminal(JobState state) noexcept;

} // namespace axk::app
#include <chrono>
