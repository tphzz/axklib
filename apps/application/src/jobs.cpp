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
    : impl_(std::make_unique<Impl>(registry, read_worker_count, write_worker_count, maximum_queued_jobs,
                                   replay_events_per_job, maximum_retained_jobs, retention, std::move(now), uploads,
                                   path_reservations)) {}

axk::app::JobManager::~JobManager() = default;

void axk::app::JobManagerTestAccess::set_before_running_claim(JobManager &manager, std::function<void()> hook) {
    const std::scoped_lock lock{manager.impl_->mutex};
    manager.impl_->before_running_claim = std::move(hook);
}

axk::app::Result<axk::app::JobSnapshot> axk::app::JobManager::submit(std::string operation_id, nlohmann::json request,
                                                                     OperationContext context,
                                                                     std::optional<std::string> idempotency_key) {
    const auto *descriptor = impl_->registry.find(operation_id);
    if (descriptor == nullptr)
        return std::unexpected(job_runtime_detail::job_error("unknown_operation", "operation is not registered"));
    if (descriptor->mode != ExecutionMode::job)
        return std::unexpected(
            job_runtime_detail::job_error("invalid_execution_mode", "operation does not run as a job"));
    if (!impl_->registry.is_implemented(operation_id))
        return std::unexpected(
            job_runtime_detail::job_error("operation_not_implemented", "operation is registered but not implemented"));
    if (context.owner_id.empty())
        return std::unexpected(job_runtime_detail::job_error("invalid_job_owner", "job owner must not be empty"));
    if (descriptor->requires_idempotency && !idempotency_key)
        return std::unexpected(
            job_runtime_detail::job_error("idempotency_key_required", "operation requires an idempotency key"));
    if (idempotency_key && (idempotency_key->empty() || idempotency_key->size() > 128U))
        return std::unexpected(
            job_runtime_detail::job_error("invalid_idempotency_key", "idempotency key must contain 1 to 128 bytes"));

    auto scheduling = impl_->registry.read_scheduling(operation_id, request, context);
    if (!scheduling)
        return std::unexpected(scheduling.error());

    std::unique_lock submission_lock{impl_->submission_mutex};
    const auto request_fingerprint = request.dump();
    const auto idempotency_index =
        idempotency_key ? std::optional<std::string>{context.owner_id + '\0' + *idempotency_key} : std::nullopt;
    if (idempotency_index) {
        std::shared_ptr<Impl::Record> replayed;
        {
            const std::scoped_lock lock{impl_->mutex};
            impl_->cleanup_expired_locked();
            const auto found = impl_->idempotent_submissions.find(*idempotency_index);
            if (found != impl_->idempotent_submissions.end()) {
                if (found->second.operation_id != operation_id ||
                    found->second.request_fingerprint != request_fingerprint) {
                    return std::unexpected(job_runtime_detail::job_error(
                        "idempotency_conflict", "idempotency key was already used for another request"));
                }
                const auto job = impl_->jobs.find(found->second.job_id);
                if (job != impl_->jobs.end())
                    replayed = job->second;
            }
        }
        if (replayed)
            return impl_->snapshot(replayed);
    }

    // Admission may fsync a device-operation fence. Never hold the runtime
    // mutex while calling a resource owner that can wait for device I/O.
    auto admission_context = context;
    admission_context.mutation_id = idempotency_key.value_or("");
    auto admitted = impl_->registry.admit_job(operation_id, request, admission_context);
    if (!admitted)
        return std::unexpected(admitted.error());
    PathReservationCoordinator::Lease path_lease;
    if (impl_->path_reservations != nullptr) {
        auto resolved_accesses = impl_->registry.path_accesses(operation_id, request, context);
        if (!resolved_accesses)
            return std::unexpected(resolved_accesses.error());
        auto acquired = impl_->path_reservations->try_acquire(*resolved_accesses);
        if (!acquired) {
            const auto has_destination = std::ranges::any_of(
                *resolved_accesses, [](const PathAccess &access) { return access.mode == PathAccessMode::exclusive; });
            if (has_destination)
                return std::unexpected(job_runtime_detail::job_error(
                    "destination_reserved", "destination is reserved by another active operation", true));
            return std::unexpected(acquired.error());
        }
        path_lease = std::move(*acquired);
    }

    auto record = std::make_shared<Impl::Record>();
    record->operation_id = std::move(operation_id);
    record->operation_class = descriptor->operation_class;
    record->request = std::move(request);
    record->context = std::move(context);
    record->scheduling = std::move(*scheduling);
    if (record->scheduling)
        record->read_group = std::to_string(record->context.owner_id.size()) + ':' + record->context.owner_id + ':' +
                             record->scheduling->scope;
    record->path_lease = std::move(path_lease);
    record->admission_lease = std::move(*admitted);
    if (impl_->uploads != nullptr) {
        std::vector<std::string> upload_ids;
        job_detail::collect_upload_ids(record->request, upload_ids);
        record->upload_leases.reserve(upload_ids.size());
        for (const auto &upload_id : upload_ids) {
            auto lease = impl_->uploads->lease(UploadRef{upload_id}, record->context.owner_id);
            if (!lease)
                return std::unexpected(lease.error());
            record->upload_leases.push_back(std::move(*lease));
        }
    }
    if (record->operation_class == OperationClass::write)
        record->destination_keys = job_detail::destination_keys(record->request);
    record->idempotency_index = idempotency_index;
    JobEvent queued_event;
    JobSnapshot initial_snapshot;
    std::shared_ptr<Impl::Record> replayed_record;
    std::vector<EventSink> sinks;
    Impl::PreemptedReads preempted;
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->cleanup_expired_locked();
        if (impl_->stopping)
            return std::unexpected(
                job_runtime_detail::job_error("job_runtime_stopping", "job runtime is stopping", true));
        if (record->idempotency_index) {
            const auto found = impl_->idempotent_submissions.find(*record->idempotency_index);
            if (found != impl_->idempotent_submissions.end()) {
                if (found->second.operation_id != record->operation_id ||
                    found->second.request_fingerprint != request_fingerprint) {
                    return std::unexpected(job_runtime_detail::job_error(
                        "idempotency_conflict", "idempotency key was already used for another request"));
                }
                const auto replayed = impl_->jobs.find(found->second.job_id);
                if (replayed != impl_->jobs.end())
                    replayed_record = replayed->second;
            }
        }
        if (!replayed_record) {
            for (const auto &candidate : record->destination_keys) {
                if (std::ranges::any_of(impl_->destination_reservations, [&](const auto &reserved) {
                        return job_detail::destinations_overlap(candidate, reserved.first);
                    })) {
                    return std::unexpected(job_runtime_detail::job_error(
                        "destination_reserved", "destination is reserved by another active job", true));
                }
            }
            impl_->prune_cancelled_locked();
            const auto candidates = impl_->preemption_candidates_locked(*record);
            const auto replaced_queued =
                static_cast<std::size_t>(std::ranges::count_if(impl_->read_queue, [&](const auto &candidate) {
                    return std::ranges::find(candidates, candidate) != candidates.end();
                }));
            if (impl_->read_queue.size() + impl_->write_queue.size() - replaced_queued >= impl_->maximum_queue)
                return std::unexpected(job_runtime_detail::job_error(
                    "job_queue_full", "job queue has reached its configured capacity", true));
            if (impl_->jobs.size() >= impl_->maximum_jobs)
                return std::unexpected(
                    job_runtime_detail::job_error("job_capacity_full", "retained job capacity is exhausted", true));
            do {
                auto job_id = impl_->next_id();
                if (!job_id)
                    return std::unexpected(job_id.error());
                record->job_id = std::move(*job_id);
            } while (impl_->jobs.contains(record->job_id));
            record->context.mutation_id = idempotency_key.value_or(record->job_id);
            {
                const std::scoped_lock record_lock{record->mutex};
                record->submitted_at = impl_->now();
                queued_event = impl_->append_event_locked(*record, "queued");
                initial_snapshot = {.job_id = record->job_id,
                                    .operation_id = record->operation_id,
                                    .state = record->state,
                                    .latest_sequence = record->latest_sequence,
                                    .progress = record->progress,
                                    .result = record->result,
                                    .error = record->error};
            }
            impl_->jobs.emplace(record->job_id, record);
            impl_->preempt_locked(candidates, preempted);
            impl_->submitted_jobs.fetch_add(1U, std::memory_order_relaxed);
            impl_->queued_jobs.fetch_add(1U, std::memory_order_relaxed);
            for (const auto &key : record->destination_keys)
                impl_->destination_reservations.emplace(key, record->job_id);
            if (record->idempotency_index) {
                impl_->idempotent_submissions.emplace(
                    *record->idempotency_index,
                    Impl::IdempotentSubmission{record->operation_id, request_fingerprint, record->job_id});
            }
            impl_->queue_for(record->operation_class).push_back(record);
            sinks.reserve(impl_->subscribers.size());
            for (const auto &[id, sink] : impl_->subscribers) {
                static_cast<void>(id);
                sinks.push_back(sink);
            }
        }
    }
    submission_lock.unlock();
    preempted.paths.clear();
    preempted.uploads.clear();
    for (const auto &event : preempted.events)
        impl_->emit(event);
    if (replayed_record)
        return impl_->snapshot(replayed_record);
    for (const auto &sink : sinks) {
        try {
            sink(queued_event);
        } catch (...) {
        }
    }
    {
        const std::scoped_lock lock{impl_->mutex};
        record->admitted = true;
    }
    impl_->condition.notify_all();
    return initial_snapshot;
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
