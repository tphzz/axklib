#include "jobs_internal.hpp"

#include <exception>

namespace axk::app {

Result<JobSnapshot> JobManager::submit(std::string operation_id, nlohmann::json request, OperationContext context,
                                       std::optional<std::string> idempotency_key) {
    // Notifications may reenter shutdown after resource admission has finished.
    // Keep their runtime alive independently of the public manager's owner.
    const auto runtime = impl_;
    {
        const std::scoped_lock lock{runtime->mutex};
        if (runtime->stopping)
            return std::unexpected(
                job_runtime_detail::job_error("job_runtime_stopping", "job runtime is stopping", true));
        ++runtime->active_submissions;
    }
    Impl::SubmissionCall call{*runtime};
    const auto *descriptor = runtime->registry.find(operation_id);
    if (!descriptor)
        return std::unexpected(job_runtime_detail::job_error("unknown_operation", "operation is not registered"));
    if (descriptor->mode != ExecutionMode::job)
        return std::unexpected(
            job_runtime_detail::job_error("invalid_execution_mode", "operation does not run as a job"));
    if (!runtime->registry.is_implemented(operation_id))
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
    auto scheduling = runtime->registry.read_scheduling(operation_id, request, context);
    if (!scheduling)
        return std::unexpected(scheduling.error());
    const auto fingerprint = request.dump();
    const auto index =
        idempotency_key ? std::optional<std::string>{context.owner_id + '\0' + *idempotency_key} : std::nullopt;
    std::shared_ptr<Impl::PendingSubmission> pending;
    {
        std::unique_lock lock{runtime->mutex};
        runtime->cleanup_expired_locked();
        if (runtime->stopping)
            return std::unexpected(
                job_runtime_detail::job_error("job_runtime_stopping", "job runtime is stopping", true));
        if (index) {
            // Committed replay wins even while its queued notification is running.
            const auto accepted = runtime->idempotent_submissions.find(*index);
            if (accepted != runtime->idempotent_submissions.end()) {
                if (accepted->second.operation_id != operation_id ||
                    accepted->second.request_fingerprint != fingerprint)
                    return std::unexpected(job_runtime_detail::job_error(
                        "idempotency_conflict", "idempotency key was already used for another request"));
                const auto found = runtime->jobs.find(accepted->second.job_id);
                if (found != runtime->jobs.end()) {
                    const auto record = found->second;
                    lock.unlock();
                    return runtime->snapshot(record);
                }
            }
            const auto found = runtime->pending_submissions.find(*index);
            if (found != runtime->pending_submissions.end()) {
                pending = found->second;
                if (pending->operation_id != operation_id || pending->request_fingerprint != fingerprint)
                    return std::unexpected(job_runtime_detail::job_error(
                        "idempotency_conflict", "idempotency key was already used for another request"));
                ++runtime->submission_waiters;
                runtime->condition.notify_all();
                runtime->condition.wait(lock, [&] { return pending->completed; });
                --runtime->submission_waiters;
                return pending->outcome;
            }
        }
        if (runtime->jobs.size() + runtime->pending_admissions >= runtime->maximum_jobs)
            return std::unexpected(
                job_runtime_detail::job_error("job_capacity_full", "retained job capacity is exhausted", true));
        pending = std::make_shared<Impl::PendingSubmission>();
        pending->operation_id = operation_id;
        pending->request_fingerprint = fingerprint;
        if (index)
            runtime->pending_submissions.emplace(*index, pending);
        ++runtime->pending_admissions;
    }

    Impl::SubmissionCompletion completion{*runtime, pending, index};
    static_assert(std::is_nothrow_move_assignable_v<Result<JobSnapshot>>);
    JobEvent queued_event;
    std::vector<EventSink> sinks;
    Impl::PreemptedReads preempted;
    std::shared_ptr<Impl::Record> record;
    bool accepted{};
    const auto prepare = [&]() -> Result<void> {
        auto admission_context = context;
        admission_context.mutation_id = idempotency_key.value_or("");
        auto admitted = runtime->registry.admit_job(operation_id, request, admission_context);
        if (!admitted)
            return std::unexpected(admitted.error());
        PathReservationCoordinator::Lease path_lease;
        if (runtime->path_reservations) {
            auto accesses = runtime->registry.path_accesses(operation_id, request, context);
            if (!accesses)
                return std::unexpected(accesses.error());
            auto acquired = runtime->path_reservations->try_acquire(*accesses);
            if (!acquired) {
                if (std::ranges::any_of(
                        *accesses, [](const PathAccess &access) { return access.mode == PathAccessMode::exclusive; }))
                    return std::unexpected(job_runtime_detail::job_error(
                        "destination_reserved", "destination is reserved by another active operation", true));
                return std::unexpected(acquired.error());
            }
            path_lease = std::move(*acquired);
        }
        record = std::make_shared<Impl::Record>();
        record->operation_id = operation_id;
        record->operation_class = descriptor->operation_class;
        record->request = std::move(request);
        record->context = std::move(context);
        record->scheduling = std::move(*scheduling);
        if (record->scheduling)
            record->read_group = std::to_string(record->context.owner_id.size()) + ':' + record->context.owner_id +
                                 ':' + record->scheduling->scope;
        record->path_lease = std::move(path_lease);
        record->admission_lease = std::move(*admitted);
        if (runtime->uploads) {
            std::vector<std::string> upload_ids;
            job_detail::collect_upload_ids(record->request, upload_ids);
            record->upload_leases.reserve(upload_ids.size());
            for (const auto &upload_id : upload_ids) {
                auto lease = runtime->uploads->lease(UploadRef{upload_id}, record->context.owner_id);
                if (!lease)
                    return std::unexpected(lease.error());
                record->upload_leases.push_back(std::move(*lease));
            }
        }
        if (record->operation_class == OperationClass::write)
            record->destination_keys = job_detail::destination_keys(record->request);
        record->idempotency_index = index;
        const std::scoped_lock lock{runtime->mutex};
        runtime->cleanup_expired_locked();
        if (runtime->stopping)
            return std::unexpected(
                job_runtime_detail::job_error("job_runtime_stopping", "job runtime is stopping", true));
        for (const auto &candidate : record->destination_keys)
            if (std::ranges::any_of(runtime->destination_reservations, [&](const auto &reserved) {
                    return job_detail::destinations_overlap(candidate, reserved.first);
                }))
                return std::unexpected(job_runtime_detail::job_error(
                    "destination_reserved", "destination is reserved by another active job", true));
        runtime->prune_cancelled_locked();
        const auto candidates = runtime->preemption_candidates_locked(*record);
        const auto replaced =
            static_cast<std::size_t>(std::ranges::count_if(runtime->read_queue, [&](const auto &candidate) {
                return std::ranges::find(candidates, candidate) != candidates.end();
            }));
        if (runtime->read_queue.size() + runtime->write_queue.size() - replaced >= runtime->maximum_queue)
            return std::unexpected(
                job_runtime_detail::job_error("job_queue_full", "job queue has reached its configured capacity", true));
        do {
            auto id = runtime->next_id();
            if (!id)
                return std::unexpected(id.error());
            record->job_id = std::move(*id);
        } while (runtime->jobs.contains(record->job_id));
        record->context.mutation_id = idempotency_key.value_or(record->job_id);
        record->submitted_at = runtime->now();
        queued_event = runtime->append_event_locked(*record, "queued");
        JobSnapshot initial{.job_id = record->job_id,
                            .operation_id = record->operation_id,
                            .state = record->state,
                            .latest_sequence = record->latest_sequence,
                            .progress = record->progress,
                            .result = record->result,
                            .error = record->error};
        sinks.reserve(runtime->subscribers.size());
        for (const auto &[id, sink] : runtime->subscribers) {
            static_cast<void>(id);
            sinks.push_back(sink);
        }
        preempted.events.reserve(candidates.size());
        preempted.queued.reserve(candidates.size());
        const auto preemption_time = runtime->now();
        for (const auto &candidate : candidates) {
            const std::scoped_lock candidate_lock{candidate->mutex};
            if (candidate->state == JobState::queued)
                preempted.queued.push_back(
                    {candidate,
                     job_runtime_detail::job_error("read_preempted", "Read yielded to foreground audio", true)});
        }
        // All potentially rejecting publication steps precede preemption. Roll
        // back partial indexes without destroying resource leases under the lock.
        try {
            runtime->jobs.emplace(record->job_id, record);
            if (runtime->before_queue_publication)
                runtime->before_queue_publication();
            for (const auto &key : record->destination_keys)
                runtime->destination_reservations.emplace(key, record->job_id);
            if (index)
                runtime->idempotent_submissions.emplace(
                    *index, Impl::IdempotentSubmission{operation_id, fingerprint, record->job_id});
            runtime->queue_for(record->operation_class).push_back(record);
        } catch (...) {
            runtime->jobs.erase(record->job_id);
            if (index)
                runtime->idempotent_submissions.erase(*index);
            for (const auto &key : record->destination_keys)
                runtime->destination_reservations.erase(key);
            throw;
        }
        --runtime->pending_admissions;
        pending->reserved = false;
        pending->outcome = Result<JobSnapshot>{std::move(initial)};
        accepted = true;
        runtime->submitted_jobs.fetch_add(1U, std::memory_order_relaxed);
        runtime->queued_jobs.fetch_add(1U, std::memory_order_relaxed);
        runtime->preempt_locked(candidates, preempted, preemption_time);
        return {};
    };
    auto outcome = [&]() -> Result<void> {
        try {
            return prepare();
        } catch (...) {
            if (accepted)
                return {};
            return std::unexpected(job_runtime_detail::job_error("job_submission_failed",
                                                                 "Job submission failed before acceptance", true));
        }
    }();
    // Refused resource admission must be fully released before followers return.
    if (!accepted) {
        record.reset();
        preempted.queued.clear();
        preempted.events.clear();
    }
    for (auto &preempted_record : preempted.queued) {
        try {
            auto event = runtime->transition(preempted_record.record, JobState::cancelled,
                                             std::move(preempted_record.event_type), std::nullopt,
                                             std::move(preempted_record.error), false);
            if (event)
                preempted.events.push_back(std::move(*event));
        } catch (...) {
            // Terminal cleanup precedes event/retention allocation. Continue
            // releasing the remaining queued reads if notification allocation fails.
        }
    }
    if (!outcome)
        pending->outcome = std::unexpected(std::move(outcome.error()));
    completion.finish();
    call.release();
    runtime->condition.notify_all();
    for (const auto &event : preempted.events)
        runtime->emit(event);
    if (!pending->outcome)
        return pending->outcome;
    for (const auto &sink : sinks) {
        try {
            sink(queued_event);
        } catch (...) {
        }
    }
    {
        const std::scoped_lock lock{runtime->mutex};
        record->admitted = true;
    }
    runtime->condition.notify_all();
    return pending->outcome;
}

} // namespace axk::app
