#pragma once

#include "axklib/application/jobs.hpp"
#include "axklib/application/secure_random.hpp"
#include "job_progress.hpp"
#include "job_request_resources.hpp"
#include "jobs_support.hpp"
#include "operation_diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

struct axk::app::JobManager::Impl {
    struct Record {
        mutable std::mutex mutex;
        bool admitted{};
        bool finishing{};
        std::string job_id;
        std::string operation_id;
        OperationClass operation_class{OperationClass::read};
        nlohmann::json request;
        std::optional<std::string> idempotency_index;
        std::vector<std::string> destination_keys;
        OperationContext context;
        JobState state{JobState::queued};
        std::uint64_t latest_sequence{};
        std::optional<JobProgress> progress;
        job_runtime_detail::ProgressPublication progress_publication;
        std::optional<nlohmann::json> result;
        std::optional<Error> error;
        std::deque<JobEvent> events;
        CancellationSource cancellation;
        bool cancellation_requested{};
        bool preempted{};
        std::optional<ReadJobScheduling> scheduling;
        std::string read_group;
        Clock::time_point submitted_at;
        std::optional<Clock::time_point> running_at;
        std::optional<Clock::time_point> phase_started_at;
        std::optional<Clock::time_point> cancellation_requested_at;
        std::vector<UploadLease> upload_leases;
        PathReservationCoordinator::Lease path_lease;
        std::shared_ptr<void> admission_lease;
    };

    struct IdempotentSubmission {
        std::string operation_id;
        std::string request_fingerprint;
        std::string job_id;
    };

    struct TerminalRecord {
        Clock::time_point completed_at;
        std::string job_id;
    };

    class ProgressAdapter final : public ProgressSink {
      public:
        ProgressAdapter(Impl &owner, std::shared_ptr<Record> record) : owner_(owner), record_(std::move(record)) {}

        void report(const Progress &progress) noexcept override {
            try {
                owner_.report_progress(record_, progress);
            } catch (...) {
            }
        }

      private:
        Impl &owner_;
        std::shared_ptr<Record> record_;
    };

    Impl(const OperationRegistry &operation_registry, std::size_t read_worker_count, std::size_t write_worker_count,
         std::size_t maximum_queued_jobs, std::size_t replay_events_per_job, std::size_t maximum_retained_jobs,
         std::chrono::seconds retention, Now now_function, UploadStore *upload_store,
         PathReservationCoordinator *reservations)
        : registry(operation_registry), maximum_queue(std::max<std::size_t>(maximum_queued_jobs, 1U)),
          maximum_replay(std::max<std::size_t>(replay_events_per_job, 1U)),
          maximum_jobs(std::max<std::size_t>(maximum_retained_jobs, 1U)), retention(retention),
          now(std::move(now_function)), uploads(upload_store), path_reservations(reservations) {
        const auto readers = std::max<std::size_t>(read_worker_count, 1U);
        const auto writers = std::max<std::size_t>(write_worker_count, 1U);
        workers.reserve(readers + writers);
        for (std::size_t index = 0; index < readers; ++index)
            workers.emplace_back([this] { worker_loop(OperationClass::read); });
        for (std::size_t index = 0; index < writers; ++index)
            workers.emplace_back([this] { worker_loop(OperationClass::write); });
    }

    ~Impl() { shutdown(); }

    Result<std::string> next_id() {
        auto suffix = secure_random_hex(16U);
        if (!suffix)
            return std::unexpected(suffix.error());
        return "job-" + std::move(*suffix);
    }

    JobSnapshot snapshot(const std::shared_ptr<Record> &record) const {
        const std::scoped_lock lock{record->mutex};
        return {.job_id = record->job_id,
                .operation_id = record->operation_id,
                .state = record->state,
                .latest_sequence = record->latest_sequence,
                .progress = record->progress,
                .result = record->result,
                .error = record->error};
    }

    JobEvent append_event_locked(Record &record, std::string type, std::optional<JobProgress> progress = std::nullopt) {
        ++record.latest_sequence;
        JobEvent event{.event_id = record.job_id + '-' + std::to_string(record.latest_sequence),
                       .sequence = record.latest_sequence,
                       .job_id = record.job_id,
                       .operation_id = record.operation_id,
                       .owner_id = record.context.owner_id,
                       .type = std::move(type),
                       .state = record.state,
                       .timestamp_unix_ms = job_runtime_detail::timestamp_unix_ms(),
                       .progress = std::move(progress)};
        record.events.push_back(event);
        while (record.events.size() > maximum_replay)
            record.events.pop_front();
        published_events.fetch_add(1U, std::memory_order_relaxed);
        return event;
    }

    void record_transition_locked(Record &record, JobState previous, JobState next) noexcept {
        const auto current = now();
        if (next == JobState::running) {
            record.running_at = current;
            queued_jobs.fetch_sub(1U, std::memory_order_relaxed);
            running_jobs.fetch_add(1U, std::memory_order_relaxed);
            total_queue_wait_ms.fetch_add(job_runtime_detail::elapsed_ms(record.submitted_at, current),
                                          std::memory_order_relaxed);
            return;
        }
        if (!is_terminal(next))
            return;
        if (previous == JobState::queued)
            queued_jobs.fetch_sub(1U, std::memory_order_relaxed);
        else if (previous == JobState::running)
            running_jobs.fetch_sub(1U, std::memory_order_relaxed);
        if (record.phase_started_at) {
            total_phase_duration_ms.fetch_add(job_runtime_detail::elapsed_ms(*record.phase_started_at, current),
                                              std::memory_order_relaxed);
            record.phase_started_at.reset();
        }
        if (record.running_at)
            total_execution_ms.fetch_add(job_runtime_detail::elapsed_ms(*record.running_at, current),
                                         std::memory_order_relaxed);
        if (record.cancellation_requested_at) {
            total_cancellation_latency_ms.fetch_add(
                job_runtime_detail::elapsed_ms(*record.cancellation_requested_at, current), std::memory_order_relaxed);
        }
        if (next == JobState::completed)
            completed_jobs.fetch_add(1U, std::memory_order_relaxed);
        else if (next == JobState::failed)
            failed_jobs.fetch_add(1U, std::memory_order_relaxed);
        else
            cancelled_jobs.fetch_add(1U, std::memory_order_relaxed);
    }

    void emit(const JobEvent &event) noexcept {
        std::vector<EventSink> sinks;
        {
            const std::scoped_lock lock{mutex};
            sinks.reserve(subscribers.size());
            for (const auto &[id, sink] : subscribers) {
                static_cast<void>(id);
                sinks.push_back(sink);
            }
        }
        for (const auto &sink : sinks) {
            try {
                sink(event);
            } catch (...) {
            }
        }
    }

    void transition(const std::shared_ptr<Record> &record, JobState state, std::string type,
                    std::optional<nlohmann::json> result = std::nullopt, std::optional<Error> error = std::nullopt) {
        std::optional<JobEvent> event;
        std::vector<UploadLease> released_upload_leases;
        PathReservationCoordinator::Lease released_path;
        std::shared_ptr<void> released_admission;
        if (is_terminal(state)) {
            {
                const std::scoped_lock lock{mutex, record->mutex};
                if (is_terminal(record->state) || record->finishing)
                    return;
                record->finishing = true;
                released_path = std::move(record->path_lease);
                released_upload_leases = std::move(record->upload_leases);
                released_admission = std::move(record->admission_lease);
            }
            // Cleanup may acquire device locks or publish a durable write fence.
            // Terminal polling and events must observe the completed cleanup.
            released_path = {};
            released_upload_leases.clear();
            released_admission.reset();
            const std::scoped_lock lock{mutex, record->mutex};
            for (const auto &key : record->destination_keys) {
                const auto found = destination_reservations.find(key);
                if (found != destination_reservations.end() && found->second == record->job_id)
                    destination_reservations.erase(found);
            }
            const auto previous = record->state;
            record->state = state;
            record->result = std::move(result);
            record->error = std::move(error);
            record_transition_locked(*record, previous, state);
            event = append_event_locked(*record, std::move(type), record->progress);
            terminal_jobs.push_back({now(), record->job_id});
        } else {
            const std::scoped_lock lock{record->mutex};
            if (is_terminal(record->state) || record->finishing)
                return;
            const auto previous = record->state;
            record->state = state;
            record->result = std::move(result);
            record->error = std::move(error);
            record_transition_locked(*record, previous, state);
            event = append_event_locked(*record, std::move(type), record->progress);
        }
        released_upload_leases.clear();
        released_admission.reset();
        emit(*event);
    }

    void report_progress(const std::shared_ptr<Record> &record, const Progress &progress) {
        std::optional<JobEvent> event;
        {
            const std::scoped_lock lock{record->mutex};
            if (record->state != JobState::running)
                return;
            JobProgress update{job_runtime_detail::progress_phase_name(progress.phase), progress.completed,
                               progress.total, progress.label};
            if (record->progress && record->progress->phase == update.phase &&
                record->progress->message == update.message && record->progress->total == update.total &&
                record->progress->completed > update.completed) {
                return;
            }
            const auto current = now();
            if (!record->progress || record->progress->phase != update.phase) {
                if (record->phase_started_at) {
                    total_phase_duration_ms.fetch_add(
                        job_runtime_detail::elapsed_ms(*record->phase_started_at, current), std::memory_order_relaxed);
                }
                record->phase_started_at = current;
            }
            record->progress = update;
            if (!record->progress_publication.admit(update, current))
                return;
            progress_events.fetch_add(1U, std::memory_order_relaxed);
            event = append_event_locked(*record, "progress", update);
        }
        emit(*event);
    }

    std::deque<std::shared_ptr<Record>> &queue_for(OperationClass operation_class) {
        return operation_class == OperationClass::write ? write_queue : read_queue;
    }

    static ReadIntent priority(const Record &record) {
        return record.scheduling ? record.scheduling->intent : ReadIntent::ordinary;
    }

    auto next_read(std::deque<std::shared_ptr<Record>> &queue) {
        auto best = queue.end();
        for (auto at = queue.begin(); at != queue.end(); ++at) {
            const std::scoped_lock lock{(*at)->mutex};
            if ((*at)->state != JobState::queued ||
                (!(*at)->read_group.empty() && active_read_groups.contains((*at)->read_group)))
                continue;
            if (best == queue.end() || priority(**at) < priority(**best))
                best = at;
        }
        return best != queue.end() && (*best)->admitted ? best : queue.end();
    }

    struct GroupLease {
        Impl &owner;
        const std::shared_ptr<Record> &record;
        ~GroupLease() {
            if (record->read_group.empty())
                return;
            {
                const std::scoped_lock lock{owner.mutex};
                const auto found = owner.active_read_groups.find(record->read_group);
                if (found != owner.active_read_groups.end() && found->second == record->job_id)
                    owner.active_read_groups.erase(found);
            }
            owner.condition.notify_all();
        }
    };

    struct PreemptedReads {
        std::vector<JobEvent> events;
        std::vector<std::vector<UploadLease>> uploads;
        std::vector<PathReservationCoordinator::Lease> paths;
    };

    void prune_cancelled_locked() {
        for (auto *queue : {&read_queue, &write_queue})
            std::erase_if(*queue, [](const auto &record) {
                const std::scoped_lock lock{record->mutex};
                return record->state != JobState::queued;
            });
    }

    std::vector<std::shared_ptr<Record>> preemption_candidates_locked(const Record &incoming) {
        std::vector<std::shared_ptr<Record>> result;
        if (incoming.read_group.empty())
            return result;
        for (const auto &[id, candidate] : jobs) {
            static_cast<void>(id);
            const std::scoped_lock lock{candidate->mutex};
            if (candidate->read_group == incoming.read_group && !candidate->cancellation_requested &&
                !is_terminal(candidate->state) && priority(*candidate) > priority(incoming))
                result.push_back(candidate);
        }
        return result;
    }

    void preempt_locked(const std::vector<std::shared_ptr<Record>> &candidates, PreemptedReads &released) {
        for (const auto &record : candidates) {
            const std::scoped_lock lock{record->mutex};
            if (is_terminal(record->state) || record->cancellation_requested)
                continue;
            record->preempted = true;
            record->cancellation_requested = true;
            record->cancellation_requested_at = now();
            record->cancellation.cancel();
            if (record->state == JobState::queued) {
                released.paths.push_back(std::move(record->path_lease));
                released.uploads.push_back(std::move(record->upload_leases));
                const auto previous = record->state;
                record->state = JobState::cancelled;
                record->error =
                    job_runtime_detail::job_error("read_preempted", "Read yielded to foreground audio", true);
                record_transition_locked(*record, previous, record->state);
                terminal_jobs.push_back({now(), record->job_id});
                released.events.push_back(append_event_locked(*record, "cancelled"));
            } else {
                released.events.push_back(append_event_locked(*record, "cancellation_requested", record->progress));
            }
        }
        prune_cancelled_locked();
    }

    bool claim_running(const std::shared_ptr<Record> &record) {
        std::optional<JobEvent> event;
        {
            const std::scoped_lock lock{mutex, record->mutex};
            if (stopping || record->state != JobState::queued || record->cancellation_requested || record->finishing)
                return false;
            const auto previous = record->state;
            record->state = JobState::running;
            record_transition_locked(*record, previous, record->state);
            event = append_event_locked(*record, "running");
        }
        emit(*event);
        return true;
    }

    void worker_loop(OperationClass operation_class) {
        for (;;) {
            std::shared_ptr<Record> record;
            std::function<void()> before_claim;
            {
                std::unique_lock lock{mutex};
                auto &queue = queue_for(operation_class);
                const auto next = [&] {
                    return operation_class == OperationClass::read
                               ? next_read(queue)
                               : (!queue.empty() && queue.front()->admitted ? queue.begin() : queue.end());
                };
                condition.wait(lock, [&] { return stopping || next() != queue.end(); });
                if (queue.empty()) {
                    if (stopping)
                        return;
                    continue;
                }
                const auto at = stopping ? queue.begin() : next();
                record = *at;
                queue.erase(at);
                if (!stopping && !record->read_group.empty())
                    active_read_groups.emplace(record->read_group, record->job_id);
                before_claim = before_running_claim;
            }

            const GroupLease group{*this, record};

            if (before_claim)
                before_claim();
            if (!claim_running(record)) {
                if (!stopping)
                    continue;
                record->cancellation.cancel();
                transition(record, JobState::cancelled, "cancelled");
                continue;
            }

            ProgressAdapter progress{*this, record};
            auto context = record->context;
            context.cancellation = record->cancellation.token();
            context.progress = &progress;

            auto result = invoke_diagnostic_job(registry, record->operation_id, record->request, std::move(context),
                                                record->job_id);
            if (result) {
                transition(record, JobState::completed, "completed", std::optional<nlohmann::json>{std::move(*result)});
            } else if (result.error().code == "operation_cancelled" || result.error().code == "cancelled") {
                std::optional<Error> reason;
                {
                    const std::scoped_lock lock{record->mutex};
                    if (record->preempted)
                        reason =
                            job_runtime_detail::job_error("read_preempted", "Read yielded to foreground audio", true);
                }
                transition(record, JobState::cancelled, "cancelled", {}, std::move(reason));
            } else {
                transition(record, JobState::failed, "failed", std::nullopt, result.error());
            }
        }
    }

    void cleanup_expired_locked() {
        const auto current = now();
        while (!terminal_jobs.empty() && terminal_jobs.front().completed_at + retention <= current) {
            const auto job_id = std::move(terminal_jobs.front().job_id);
            terminal_jobs.pop_front();
            const auto found = jobs.find(job_id);
            if (found == jobs.end())
                continue;
            if (found->second->idempotency_index) {
                const auto idempotency = idempotent_submissions.find(*found->second->idempotency_index);
                if (idempotency != idempotent_submissions.end() && idempotency->second.job_id == job_id)
                    idempotent_submissions.erase(idempotency);
            }
            jobs.erase(found);
        }
    }

    void shutdown() noexcept {
        std::vector<std::shared_ptr<Record>> active;
        {
            const std::scoped_lock lock{mutex};
            if (stopping && workers.empty())
                return;
            stopping = true;
            active.reserve(jobs.size());
            for (const auto &[id, record] : jobs) {
                static_cast<void>(id);
                active.push_back(record);
            }
        }
        for (const auto &record : active)
            record->cancellation.cancel();
        condition.notify_all();
        for (auto &worker : workers) {
            if (worker.joinable())
                worker.join();
        }
        workers.clear();
    }

    const OperationRegistry &registry;
    const std::size_t maximum_queue;
    const std::size_t maximum_replay;
    const std::size_t maximum_jobs;
    const std::chrono::seconds retention;
    Now now;
    UploadStore *uploads{};
    PathReservationCoordinator *path_reservations{};
    std::mutex submission_mutex;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<std::string, std::shared_ptr<Record>> jobs;
    std::unordered_map<std::string, IdempotentSubmission> idempotent_submissions;
    std::unordered_map<std::string, std::string> destination_reservations;
    std::deque<TerminalRecord> terminal_jobs;
    std::deque<std::shared_ptr<Record>> read_queue;
    std::deque<std::shared_ptr<Record>> write_queue;
    std::unordered_map<std::string, std::string> active_read_groups;
    std::unordered_map<SubscriptionId, EventSink> subscribers;
    std::function<void()> before_running_claim;
    std::vector<std::thread> workers;
    SubscriptionId next_subscription{1U};
    std::atomic_bool stopping{};
    std::atomic<std::uint64_t> submitted_jobs{};
    std::atomic<std::uint64_t> queued_jobs{};
    std::atomic<std::uint64_t> running_jobs{};
    std::atomic<std::uint64_t> completed_jobs{};
    std::atomic<std::uint64_t> failed_jobs{};
    std::atomic<std::uint64_t> cancelled_jobs{};
    std::atomic<std::uint64_t> published_events{};
    std::atomic<std::uint64_t> progress_events{};
    std::atomic<std::uint64_t> total_queue_wait_ms{};
    std::atomic<std::uint64_t> total_execution_ms{};
    std::atomic<std::uint64_t> total_phase_duration_ms{};
    std::atomic<std::uint64_t> total_cancellation_latency_ms{};
};
