#include "axklib/application/jobs.hpp"
#include "operation_diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "axklib/application/secure_random.hpp"
#include "job_progress.hpp"
#include "job_request_resources.hpp"
#include "jobs_support.hpp"
#include "jobs_test_access.hpp"

#include "jobs_internal.hpp"
axk::app::JobManager::JobManager(const OperationRegistry &registry, std::size_t read_worker_count,
                                 std::size_t write_worker_count, std::size_t maximum_queued_jobs,
                                 std::size_t replay_events_per_job, std::size_t maximum_retained_jobs,
                                 std::chrono::seconds retention, Now now, UploadStore *uploads,
                                 PathReservationCoordinator *path_reservations)
    : impl_(std::make_shared<Impl>(registry, read_worker_count, write_worker_count, maximum_queued_jobs,
                                   replay_events_per_job, maximum_retained_jobs, retention, std::move(now), uploads,
                                   path_reservations)) {}

axk::app::JobManager::~JobManager() { impl_->shutdown(); }

void axk::app::JobManagerTestAccess::set_before_running_claim(JobManager &manager, std::function<void()> hook) {
    const std::scoped_lock lock{manager.impl_->mutex};
    manager.impl_->before_running_claim = std::move(hook);
}

void axk::app::JobManagerTestAccess::set_before_queue_publication(JobManager &manager, std::function<void()> hook) {
    const std::scoped_lock lock{manager.impl_->mutex};
    manager.impl_->before_queue_publication = std::move(hook);
}

bool axk::app::JobManagerTestAccess::wait_for_submission_waiters(JobManager &manager, std::size_t count,
                                                                 std::chrono::milliseconds timeout) {
    std::unique_lock lock{manager.impl_->mutex};
    return manager.impl_->condition.wait_for(lock, timeout, [&] { return manager.impl_->submission_waiters >= count; });
}

bool axk::app::JobManagerTestAccess::wait_until_stopping(JobManager &manager, std::chrono::milliseconds timeout) {
    std::unique_lock lock{manager.impl_->mutex};
    return manager.impl_->condition.wait_for(lock, timeout, [&] { return manager.impl_->stopping.load(); });
}

void axk::app::JobManagerTestAccess::set_before_terminal_cleanup(JobManager &manager,
                                                                 std::function<void(std::string_view)> hook) {
    const std::scoped_lock lock{manager.impl_->mutex};
    manager.impl_->before_terminal_cleanup = std::move(hook);
}

axk::app::Result<axk::app::JobSnapshot> axk::app::JobManager::status(std::string_view job_id,
                                                                     std::string_view owner_id) const {
    std::shared_ptr<Impl::Record> record;
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->cleanup_expired_locked();
        const auto found = impl_->jobs.find(std::string{job_id});
        if (found == impl_->jobs.end() || found->second->context.owner_id != owner_id)
            return std::unexpected(job_runtime_detail::job_error("job_not_found", "job is closed or unknown"));
        record = found->second;
    }
    return impl_->snapshot(record);
}

axk::app::Result<void> axk::app::JobManager::cancel(std::string_view job_id, std::string_view owner_id) {
    std::shared_ptr<Impl::Record> record;
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->cleanup_expired_locked();
        const auto found = impl_->jobs.find(std::string{job_id});
        if (found == impl_->jobs.end() || found->second->context.owner_id != owner_id)
            return std::unexpected(job_runtime_detail::job_error("job_not_found", "job is closed or unknown"));
        record = found->second;
    }

    std::optional<JobEvent> event;
    bool queued{};
    {
        const std::scoped_lock lock{impl_->mutex, record->mutex};
        if (is_terminal(record->state) || record->cancellation_requested || record->finishing)
            return {};
        record->cancellation_requested = true;
        record->cancellation_requested_at = impl_->now();
        record->cancellation.cancel();
        if (record->state == JobState::queued) {
            queued = true;
        } else {
            event = impl_->append_event_locked(*record, "cancellation_requested", record->progress);
        }
    }
    if (queued)
        impl_->transition(record, JobState::cancelled, "cancelled");
    else if (event)
        impl_->emit(*event);
    impl_->condition.notify_all();
    return {};
}

axk::app::Result<std::vector<axk::app::JobEvent>>
axk::app::JobManager::replay(std::string_view job_id, std::string_view owner_id, std::uint64_t after_sequence) const {
    std::shared_ptr<Impl::Record> record;
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->cleanup_expired_locked();
        const auto found = impl_->jobs.find(std::string{job_id});
        if (found == impl_->jobs.end() || found->second->context.owner_id != owner_id)
            return std::unexpected(job_runtime_detail::job_error("job_not_found", "job is closed or unknown"));
        record = found->second;
    }
    const std::scoped_lock lock{record->mutex};
    if (!record->events.empty() && after_sequence < record->events.front().sequence - 1U) {
        return std::unexpected(
            job_runtime_detail::job_error("job_event_replay_expired", "requested job events are no longer retained"));
    }
    std::vector<JobEvent> events;
    for (const auto &event : record->events) {
        if (event.sequence > after_sequence)
            events.push_back(event);
    }
    return events;
}

axk::app::JobRuntimeMetrics axk::app::JobManager::metrics() const noexcept {
    return {.submitted_jobs = impl_->submitted_jobs.load(std::memory_order_relaxed),
            .queued_jobs = impl_->queued_jobs.load(std::memory_order_relaxed),
            .running_jobs = impl_->running_jobs.load(std::memory_order_relaxed),
            .completed_jobs = impl_->completed_jobs.load(std::memory_order_relaxed),
            .failed_jobs = impl_->failed_jobs.load(std::memory_order_relaxed),
            .cancelled_jobs = impl_->cancelled_jobs.load(std::memory_order_relaxed),
            .published_events = impl_->published_events.load(std::memory_order_relaxed),
            .progress_events = impl_->progress_events.load(std::memory_order_relaxed),
            .total_queue_wait_ms = impl_->total_queue_wait_ms.load(std::memory_order_relaxed),
            .total_execution_ms = impl_->total_execution_ms.load(std::memory_order_relaxed),
            .total_phase_duration_ms = impl_->total_phase_duration_ms.load(std::memory_order_relaxed),
            .total_cancellation_latency_ms = impl_->total_cancellation_latency_ms.load(std::memory_order_relaxed)};
}

axk::app::JobManager::SubscriptionId axk::app::JobManager::subscribe(EventSink sink) {
    if (!sink)
        return 0U;
    const std::scoped_lock lock{impl_->mutex};
    const auto id = impl_->next_subscription++;
    impl_->subscribers.emplace(id, std::move(sink));
    return id;
}

void axk::app::JobManager::unsubscribe(SubscriptionId subscription_id) noexcept {
    const std::scoped_lock lock{impl_->mutex};
    impl_->subscribers.erase(subscription_id);
}

void axk::app::JobManager::shutdown() noexcept { impl_->shutdown(); }
