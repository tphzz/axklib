#include "axklib/application/jobs.hpp"

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

namespace {

using Json = nlohmann::json;

std::uint64_t timestamp_unix_ms() {
    const auto duration = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

std::string progress_phase_name(axk::ProgressPhase phase) {
    switch (phase) {
    case axk::ProgressPhase::opening:
        return "opening";
    case axk::ProgressPhase::reading:
        return "reading";
    case axk::ProgressPhase::resolving:
        return "resolving";
    case axk::ProgressPhase::validating:
        return "validating";
    case axk::ProgressPhase::exporting:
        return "exporting";
    case axk::ProgressPhase::writing:
        return "writing";
    case axk::ProgressPhase::allocating:
        return "allocating";
    case axk::ProgressPhase::publishing:
        return "publishing";
    }
    return "unknown";
}

axk::app::Error job_error(std::string code, std::string message, bool retryable = false) {
    return {std::move(code), std::move(message), {}, retryable};
}

std::optional<std::string> destination_key(const Json &request, std::string_view member) {
    const auto found = request.find(member);
    if (found == request.end() || !found->is_object())
        return std::nullopt;
    const auto root = found->find("rootId");
    const auto relative = found->find("relativePath");
    if (root == found->end() || relative == found->end() || !root->is_string() || !relative->is_string())
        return std::nullopt;

    std::string normalized;
    std::string_view remaining{relative->get_ref<const std::string &>()};
    while (!remaining.empty()) {
        const auto separator = remaining.find('/');
        const auto component = remaining.substr(0U, separator);
        if (!component.empty() && component != ".") {
            if (!normalized.empty())
                normalized.push_back('/');
            normalized.append(component);
        }
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1U);
    }
    return root->get<std::string>() + '\0' + normalized;
}

std::vector<std::string> destination_keys(const Json &request) {
    std::vector<std::string> result;
    for (const auto member : {"destination", "output"}) {
        if (auto key = destination_key(request, member); key && std::ranges::find(result, *key) == result.end())
            result.push_back(std::move(*key));
    }
    return result;
}

bool destinations_overlap(std::string_view left, std::string_view right) {
    const auto left_separator = left.find('\0');
    const auto right_separator = right.find('\0');
    if (left_separator == std::string_view::npos || right_separator == std::string_view::npos ||
        left.substr(0U, left_separator) != right.substr(0U, right_separator)) {
        return false;
    }
    left.remove_prefix(left_separator + 1U);
    right.remove_prefix(right_separator + 1U);
    if (left == right)
        return true;
    const auto is_parent = [](std::string_view parent, std::string_view child) {
        return !parent.empty() && child.size() > parent.size() && child.starts_with(parent) &&
               child[parent.size()] == '/';
    };
    return is_parent(left, right) || is_parent(right, left);
}

bool references_root(const nlohmann::json &value, std::string_view root_id) {
    if (value.is_object()) {
        if (const auto found = value.find("rootId");
            found != value.end() && found->is_string() && found->get_ref<const std::string &>() == root_id) {
            return true;
        }
        return std::ranges::any_of(value.items(),
                                   [&](const auto &item) { return references_root(item.value(), root_id); });
    }
    if (value.is_array())
        return std::ranges::any_of(value, [&](const auto &item) { return references_root(item, root_id); });
    return false;
}

bool paths_overlap(std::string_view left, std::string_view right) {
    const auto contains = [](std::string_view parent, std::string_view child) {
        return parent.empty() || child == parent ||
               (child.starts_with(parent) && child.size() > parent.size() && child[parent.size()] == '/');
    };
    return contains(left, right) || contains(right, left);
}

bool references_path(const nlohmann::json &value, const axk::app::FileRef &reference) {
    if (value.is_object()) {
        const auto root = value.find("rootId");
        const auto path = value.find("relativePath");
        if (root != value.end() && path != value.end() && root->is_string() && path->is_string() &&
            root->get_ref<const std::string &>() == reference.root_id &&
            paths_overlap(path->get_ref<const std::string &>(), reference.relative_path)) {
            return true;
        }
        return std::ranges::any_of(value.items(),
                                   [&](const auto &item) { return references_path(item.value(), reference); });
    }
    if (value.is_array())
        return std::ranges::any_of(value, [&](const auto &item) { return references_path(item, reference); });
    return false;
}

} // namespace

struct axk::app::JobManager::Impl {
    struct Record {
        mutable std::mutex mutex;
        bool admitted{};
        std::string job_id;
        std::string operation_id;
        OperationClass operation_class{OperationClass::read};
        Json request;
        std::optional<std::string> idempotency_index;
        std::vector<std::string> destination_keys;
        OperationContext context;
        JobState state{JobState::queued};
        std::uint64_t latest_sequence{};
        std::optional<JobProgress> progress;
        std::optional<Json> result;
        std::optional<Error> error;
        std::deque<JobEvent> events;
        CancellationSource cancellation;
        bool cancellation_requested{};
        Clock::time_point submitted_at;
        std::optional<Clock::time_point> running_at;
        std::optional<Clock::time_point> phase_started_at;
        std::optional<Clock::time_point> cancellation_requested_at;
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
         std::chrono::seconds retention, Now now_function)
        : registry(operation_registry), maximum_queue(std::max<std::size_t>(maximum_queued_jobs, 1U)),
          maximum_replay(std::max<std::size_t>(replay_events_per_job, 1U)),
          maximum_jobs(std::max<std::size_t>(maximum_retained_jobs, 1U)), retention(retention),
          now(std::move(now_function)) {
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
                       .timestamp_unix_ms = timestamp_unix_ms(),
                       .progress = std::move(progress)};
        record.events.push_back(event);
        while (record.events.size() > maximum_replay)
            record.events.pop_front();
        published_events.fetch_add(1U, std::memory_order_relaxed);
        return event;
    }

    static std::uint64_t elapsed_ms(Clock::time_point start, Clock::time_point end) noexcept {
        return static_cast<std::uint64_t>(
            std::max<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), 0));
    }

    void record_transition_locked(Record &record, JobState previous, JobState next) noexcept {
        const auto current = now();
        if (next == JobState::running) {
            record.running_at = current;
            queued_jobs.fetch_sub(1U, std::memory_order_relaxed);
            running_jobs.fetch_add(1U, std::memory_order_relaxed);
            total_queue_wait_ms.fetch_add(elapsed_ms(record.submitted_at, current), std::memory_order_relaxed);
            return;
        }
        if (!is_terminal(next))
            return;
        if (previous == JobState::queued)
            queued_jobs.fetch_sub(1U, std::memory_order_relaxed);
        else if (previous == JobState::running)
            running_jobs.fetch_sub(1U, std::memory_order_relaxed);
        if (record.phase_started_at) {
            total_phase_duration_ms.fetch_add(elapsed_ms(*record.phase_started_at, current), std::memory_order_relaxed);
            record.phase_started_at.reset();
        }
        if (record.running_at)
            total_execution_ms.fetch_add(elapsed_ms(*record.running_at, current), std::memory_order_relaxed);
        if (record.cancellation_requested_at) {
            total_cancellation_latency_ms.fetch_add(elapsed_ms(*record.cancellation_requested_at, current),
                                                    std::memory_order_relaxed);
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
                    std::optional<Json> result = std::nullopt, std::optional<Error> error = std::nullopt) {
        std::optional<JobEvent> event;
        {
            const std::scoped_lock lock{record->mutex};
            if (is_terminal(record->state))
                return;
            const auto previous = record->state;
            record->state = state;
            record->result = std::move(result);
            record->error = std::move(error);
            record_transition_locked(*record, previous, state);
            event = append_event_locked(*record, std::move(type), record->progress);
        }
        if (is_terminal(state))
            retain_terminal(record);
        emit(*event);
    }

    void report_progress(const std::shared_ptr<Record> &record, const Progress &progress) {
        std::optional<JobEvent> event;
        {
            const std::scoped_lock lock{record->mutex};
            if (record->state != JobState::running)
                return;
            JobProgress update{progress_phase_name(progress.phase), progress.completed, progress.total, progress.label};
            if (record->progress && record->progress->phase == update.phase &&
                record->progress->completed > update.completed) {
                return;
            }
            const auto current = now();
            if (!record->progress || record->progress->phase != update.phase) {
                if (record->phase_started_at) {
                    total_phase_duration_ms.fetch_add(elapsed_ms(*record->phase_started_at, current),
                                                      std::memory_order_relaxed);
                }
                record->phase_started_at = current;
            }
            record->progress = update;
            progress_events.fetch_add(1U, std::memory_order_relaxed);
            event = append_event_locked(*record, "progress", update);
        }
        emit(*event);
    }

    std::deque<std::shared_ptr<Record>> &queue_for(OperationClass operation_class) {
        return operation_class == OperationClass::write ? write_queue : read_queue;
    }

    void worker_loop(OperationClass operation_class) {
        for (;;) {
            std::shared_ptr<Record> record;
            {
                std::unique_lock lock{mutex};
                auto &queue = queue_for(operation_class);
                condition.wait(lock, [&] { return stopping || (!queue.empty() && queue.front()->admitted); });
                if (queue.empty()) {
                    if (stopping)
                        return;
                    continue;
                }
                record = std::move(queue.front());
                queue.pop_front();
            }

            {
                const std::scoped_lock lock{record->mutex};
                if (is_terminal(record->state))
                    continue;
            }
            if (stopping) {
                record->cancellation.cancel();
                transition(record, JobState::cancelled, "cancelled");
                continue;
            }

            transition(record, JobState::running, "running");
            ProgressAdapter progress{*this, record};
            auto context = record->context;
            context.cancellation = record->cancellation.token();
            context.progress = &progress;

            Result<Json> result = std::unexpected(job_error("operation_failed", "operation failed"));
            try {
                result = registry.invoke(record->operation_id, record->request, context);
            } catch (...) {
                result = std::unexpected(job_error("operation_exception", "operation raised an unexpected exception"));
            }
            if (record->cancellation.token().is_cancelled()) {
                transition(record, JobState::cancelled, "cancelled");
            } else if (result) {
                transition(record, JobState::completed, "completed", std::optional<Json>{std::move(*result)});
            } else {
                transition(record, JobState::failed, "failed", std::nullopt, result.error());
            }
        }
    }

    void retain_terminal(const std::shared_ptr<Record> &record) {
        const std::scoped_lock lock{mutex};
        for (const auto &key : record->destination_keys) {
            const auto found = destination_reservations.find(key);
            if (found != destination_reservations.end() && found->second == record->job_id)
                destination_reservations.erase(found);
        }
        terminal_jobs.push_back({now(), record->job_id});
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
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<std::string, std::shared_ptr<Record>> jobs;
    std::unordered_map<std::string, IdempotentSubmission> idempotent_submissions;
    std::unordered_map<std::string, std::string> destination_reservations;
    std::deque<TerminalRecord> terminal_jobs;
    std::deque<std::shared_ptr<Record>> read_queue;
    std::deque<std::shared_ptr<Record>> write_queue;
    std::unordered_map<SubscriptionId, EventSink> subscribers;
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

axk::app::JobManager::JobManager(const OperationRegistry &registry, std::size_t read_worker_count,
                                 std::size_t write_worker_count, std::size_t maximum_queued_jobs,
                                 std::size_t replay_events_per_job, std::size_t maximum_retained_jobs,
                                 std::chrono::seconds retention, Now now)
    : impl_(std::make_unique<Impl>(registry, read_worker_count, write_worker_count, maximum_queued_jobs,
                                   replay_events_per_job, maximum_retained_jobs, retention, std::move(now))) {}

axk::app::JobManager::~JobManager() = default;

axk::app::Result<axk::app::JobSnapshot> axk::app::JobManager::submit(std::string operation_id, nlohmann::json request,
                                                                     OperationContext context,
                                                                     std::optional<std::string> idempotency_key) {
    const auto *descriptor = impl_->registry.find(operation_id);
    if (descriptor == nullptr)
        return std::unexpected(job_error("unknown_operation", "operation is not registered"));
    if (descriptor->mode != ExecutionMode::job)
        return std::unexpected(job_error("invalid_execution_mode", "operation does not run as a job"));
    if (!impl_->registry.is_implemented(operation_id))
        return std::unexpected(job_error("operation_not_implemented", "operation is registered but not implemented"));
    if (context.owner_id.empty())
        return std::unexpected(job_error("invalid_job_owner", "job owner must not be empty"));
    if (descriptor->requires_idempotency && !idempotency_key)
        return std::unexpected(job_error("idempotency_key_required", "operation requires an idempotency key"));
    if (idempotency_key && (idempotency_key->empty() || idempotency_key->size() > 128U))
        return std::unexpected(job_error("invalid_idempotency_key", "idempotency key must contain 1 to 128 bytes"));

    auto record = std::make_shared<Impl::Record>();
    record->operation_id = std::move(operation_id);
    record->operation_class = descriptor->operation_class;
    record->request = std::move(request);
    record->context = std::move(context);
    if (record->operation_class == OperationClass::write)
        record->destination_keys = destination_keys(record->request);
    const auto request_fingerprint = record->request.dump();
    if (idempotency_key)
        record->idempotency_index = record->context.owner_id + '\0' + *idempotency_key;
    JobEvent queued_event;
    JobSnapshot initial_snapshot;
    std::shared_ptr<Impl::Record> replayed_record;
    std::vector<EventSink> sinks;
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->cleanup_expired_locked();
        if (impl_->stopping)
            return std::unexpected(job_error("job_runtime_stopping", "job runtime is stopping", true));
        if (record->idempotency_index) {
            const auto found = impl_->idempotent_submissions.find(*record->idempotency_index);
            if (found != impl_->idempotent_submissions.end()) {
                if (found->second.operation_id != record->operation_id ||
                    found->second.request_fingerprint != request_fingerprint) {
                    return std::unexpected(
                        job_error("idempotency_conflict", "idempotency key was already used for another request"));
                }
                const auto replayed = impl_->jobs.find(found->second.job_id);
                if (replayed != impl_->jobs.end())
                    replayed_record = replayed->second;
            }
        }
        if (!replayed_record) {
            for (const auto &candidate : record->destination_keys) {
                if (std::ranges::any_of(impl_->destination_reservations, [&](const auto &reserved) {
                        return destinations_overlap(candidate, reserved.first);
                    })) {
                    return std::unexpected(
                        job_error("destination_reserved", "destination is reserved by another active job", true));
                }
            }
            if (impl_->read_queue.size() + impl_->write_queue.size() >= impl_->maximum_queue)
                return std::unexpected(
                    job_error("job_queue_full", "job queue has reached its configured capacity", true));
            if (impl_->jobs.size() >= impl_->maximum_jobs)
                return std::unexpected(job_error("job_capacity_full", "retained job capacity is exhausted", true));
            do {
                auto job_id = impl_->next_id();
                if (!job_id)
                    return std::unexpected(job_id.error());
                record->job_id = std::move(*job_id);
            } while (impl_->jobs.contains(record->job_id));
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
            return std::unexpected(job_error("job_not_found", "job is closed or unknown"));
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
            return std::unexpected(job_error("job_not_found", "job is closed or unknown"));
        record = found->second;
    }

    std::optional<JobEvent> event;
    {
        const std::scoped_lock lock{record->mutex};
        if (is_terminal(record->state) || record->cancellation_requested)
            return {};
        record->cancellation_requested = true;
        record->cancellation_requested_at = impl_->now();
        record->cancellation.cancel();
        if (record->state == JobState::queued) {
            const auto previous = record->state;
            record->state = JobState::cancelled;
            impl_->record_transition_locked(*record, previous, record->state);
            event = impl_->append_event_locked(*record, "cancelled");
        } else {
            event = impl_->append_event_locked(*record, "cancellation_requested", record->progress);
        }
    }
    if (event->state == JobState::cancelled)
        impl_->retain_terminal(record);
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
            return std::unexpected(job_error("job_not_found", "job is closed or unknown"));
        record = found->second;
    }
    const std::scoped_lock lock{record->mutex};
    if (!record->events.empty() && after_sequence < record->events.front().sequence - 1U) {
        return std::unexpected(job_error("job_event_replay_expired", "requested job events are no longer retained"));
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

bool axk::app::JobManager::root_in_use(std::string_view root_id) const {
    const std::scoped_lock lock{impl_->mutex};
    return std::ranges::any_of(impl_->jobs, [&](const auto &entry) {
        const std::scoped_lock record_lock{entry.second->mutex};
        return !is_terminal(entry.second->state) && references_root(entry.second->request, root_id);
    });
}

bool axk::app::JobManager::path_in_use(const FileRef &reference) const {
    const std::scoped_lock lock{impl_->mutex};
    return std::ranges::any_of(impl_->jobs, [&](const auto &entry) {
        const std::scoped_lock record_lock{entry.second->mutex};
        return !is_terminal(entry.second->state) && references_path(entry.second->request, reference);
    });
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

std::string_view axk::app::job_state_name(JobState state) noexcept {
    switch (state) {
    case JobState::queued:
        return "QUEUED";
    case JobState::running:
        return "RUNNING";
    case JobState::completed:
        return "COMPLETED";
    case JobState::failed:
        return "FAILED";
    case JobState::cancelled:
        return "CANCELLED";
    }
    return "FAILED";
}

bool axk::app::is_terminal(JobState state) noexcept {
    return state == JobState::completed || state == JobState::failed || state == JobState::cancelled;
}
