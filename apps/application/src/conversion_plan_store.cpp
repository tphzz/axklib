#include "conversion_plan_store.hpp"

#include <algorithm>
#include <utility>

#include "axklib/application/secure_random.hpp"

namespace axk::app::conversion_internal {
namespace {
Error unavailable() { return {"conversion_plan_unavailable", "Conversion plan is unavailable; prepare a new plan"}; }
Error limit() { return {"conversion_limit_exceeded", "Conversion plan retention limit reached", {}, true}; }
std::uint64_t unix_expiry(std::chrono::milliseconds retention) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          (std::chrono::system_clock::now() + retention).time_since_epoch())
                                          .count());
}
} // namespace

PlanStore::PlanStore() : PlanStore(Limits{}) {}
PlanStore::PlanStore(Limits limits, Now now) : limits_(limits), now_(std::move(now)) {}

void PlanStore::expire() {
    const auto now = now_();
    std::erase_if(records_, [now](const auto &entry) {
        return entry.second.expires <= now &&
               (entry.second.state == State::ready || entry.second.state == State::terminal);
    });
}
void PlanStore::collect_expired() {
    const std::scoped_lock lock{mutex_};
    expire();
}

Result<std::string> PlanStore::reserve(std::string owner) {
    const std::scoped_lock lock{mutex_};
    expire();
    if (owner.empty())
        return std::unexpected(Error{"invalid_request", "Conversion plan requires an owner"});
    if (pending_.size() + records_.size() >= limits_.plans || limits_.plan_bytes > limits_.total_bytes)
        return std::unexpected(limit());
    std::uint64_t bytes = pending_.size() * limits_.plan_bytes;
    for (const auto &[id, record] : records_) {
        static_cast<void>(id);
        if (record.payload) {
            const auto size = record.payload->retained_bytes();
            if (size > limits_.total_bytes - bytes)
                return std::unexpected(limit());
            bytes += size;
        }
    }
    if (limits_.plan_bytes > limits_.total_bytes - bytes)
        return std::unexpected(limit());
    auto random = secure_random_hex(16U);
    if (!random)
        return std::unexpected(random.error());
    auto id = "conversion-" + *random;
    pending_.emplace(id, std::move(owner));
    return id;
}

Result<Record> PlanStore::publish(std::string_view id, std::string_view owner, std::shared_ptr<const Payload> payload) {
    const std::scoped_lock lock{mutex_};
    const auto pending = pending_.find(std::string{id});
    if (pending == pending_.end() || pending->second != owner)
        return std::unexpected(unavailable());
    if (!payload || !payload->prepared || payload->source_retention_bytes > limits_.plan_bytes ||
        payload->prepared->retained_bytes() > limits_.plan_bytes - payload->source_retention_bytes)
        return std::unexpected(limit());
    Record record{std::string{id},
                  std::string{owner},
                  now_() + limits_.retention,
                  unix_expiry(limits_.retention),
                  State::ready,
                  {},
                  std::move(payload)};
    records_.emplace(record.id, record);
    pending_.erase(pending);
    return record;
}

void PlanStore::abandon(std::string_view id, std::string_view owner) {
    const std::scoped_lock lock{mutex_};
    const auto found = pending_.find(std::string{id});
    if (found != pending_.end() && found->second == owner)
        pending_.erase(found);
}

Result<Record> PlanStore::get(std::string_view id, std::string_view owner) {
    const std::scoped_lock lock{mutex_};
    expire();
    const auto found = records_.find(std::string{id});
    if (found == records_.end() || found->second.owner != owner)
        return std::unexpected(unavailable());
    return found->second;
}

Result<void> PlanStore::release(std::string_view id, std::string_view owner) {
    const std::scoped_lock lock{mutex_};
    expire();
    const auto found = records_.find(std::string{id});
    if (found == records_.end() || found->second.owner != owner)
        return std::unexpected(unavailable());
    if (found->second.state != State::ready)
        return std::unexpected(Error{"conversion_already_submitted", "Use the retained job to check execution status"});
    records_.erase(found);
    return {};
}

Result<std::string> PlanStore::submit(std::string_view id, std::string_view owner,
                                      std::span<const std::string> acknowledgements, const Submit &submit_job) {
    const std::scoped_lock lock{mutex_};
    expire();
    const auto found = records_.find(std::string{id});
    if (found == records_.end() || found->second.owner != owner)
        return std::unexpected(unavailable());
    auto &record = found->second;
    if (record.state != State::ready)
        return record.job_id;
    if (auto admitted = record.payload->prepared->acknowledge(acknowledgements); !admitted)
        return std::unexpected(Error{"conversion_review_required", admitted.error().message});
    // A queued worker may start immediately, but cannot claim the payload until publication below.
    auto job = submit_job();
    if (!job)
        return std::unexpected(job.error());
    record.job_id = *job;
    record.state = State::accepted;
    return job;
}

Result<std::shared_ptr<const Payload>> PlanStore::start(std::string_view id, std::string_view owner) {
    const std::scoped_lock lock{mutex_};
    const auto found = records_.find(std::string{id});
    if (found == records_.end() || found->second.owner != owner || found->second.state != State::accepted)
        return std::unexpected(unavailable());
    found->second.state = State::executing;
    return found->second.payload;
}

void PlanStore::finish(std::string_view id, std::string_view owner) {
    const std::scoped_lock lock{mutex_};
    const auto found = records_.find(std::string{id});
    if (found != records_.end() && found->second.owner == owner &&
        (found->second.state == State::accepted || found->second.state == State::executing)) {
        found->second.payload.reset();
        found->second.state = State::terminal;
        found->second.expires = now_() + limits_.retention;
        found->second.expires_unix_ms = unix_expiry(limits_.retention);
    }
}

void PlanStore::finish_job(std::string_view job_id, std::string_view owner) {
    const std::scoped_lock lock{mutex_};
    for (auto &[id, record] : records_) {
        static_cast<void>(id);
        if (record.job_id == job_id && record.owner == owner &&
            (record.state == State::accepted || record.state == State::executing)) {
            record.payload.reset();
            record.state = State::terminal;
            record.expires = now_() + limits_.retention;
            record.expires_unix_ms = unix_expiry(limits_.retention);
        }
    }
}

} // namespace axk::app::conversion_internal
