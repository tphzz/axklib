#include "package_plan_store.hpp"

#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace {

using axk::app::Error;
using axk::app::Result;
using axk::app::package_plan_internal::Clock;
using axk::app::package_plan_internal::Store;

Error plan_error(std::string code, std::string message, bool retryable = false) {
    return {std::move(code), std::move(message), {}, retryable};
}

std::string normalized_path(const std::filesystem::path &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).generic_string();
}

void cleanup(Store &store, Clock::time_point now) {
    for (auto current = store.plans.begin(); current != store.plans.end();) {
        if (!current->second->claimed && current->second->expires_at <= now) {
            const auto reservation = normalized_path(current->second->output_path);
            if (const auto found = store.destination_reservations.find(reservation);
                found != store.destination_reservations.end() && found->second == current->first) {
                store.destination_reservations.erase(found);
            }
            current = store.plans.erase(current);
        } else {
            ++current;
        }
    }
}

std::uint64_t retained_source_bytes(const Store &store) {
    std::uint64_t total{};
    for (const auto &[token, record] : store.plans) {
        static_cast<void>(token);
        if (record->source_bytes > std::numeric_limits<std::uint64_t>::max() - total)
            return std::numeric_limits<std::uint64_t>::max();
        total += record->source_bytes;
    }
    return total;
}

} // namespace

axk::app::package_plan_internal::Admission::Admission(std::shared_ptr<Store> store, std::uint64_t source_bytes)
    : store_(std::move(store)), source_bytes_(source_bytes) {}

axk::app::package_plan_internal::Admission::~Admission() { release(); }

axk::app::package_plan_internal::Admission::Admission(Admission &&other) noexcept
    : store_(std::move(other.store_)), source_bytes_(other.source_bytes_),
      active_(std::exchange(other.active_, false)) {}

axk::app::Result<void> axk::app::package_plan_internal::Admission::commit(const std::shared_ptr<Record> &record) {
    std::lock_guard lock{store_->mutex};
    cleanup(*store_, Clock::now());
    const auto reservation = normalized_path(record->output_path);
    if (store_->destination_reservations.contains(reservation)) {
        return std::unexpected(plan_error("destination_reserved", "destination is reserved by another active plan"));
    }
    if (store_->plans.contains(record->token))
        return std::unexpected(plan_error("secure_random_failed", "package plan token collision"));
    store_->destination_reservations.emplace(reservation, record->token);
    store_->plans.emplace(record->token, record);
    --store_->pending_plans;
    store_->pending_source_bytes -= source_bytes_;
    active_ = false;
    return {};
}

void axk::app::package_plan_internal::Admission::release() noexcept {
    if (!active_)
        return;
    std::lock_guard lock{store_->mutex};
    --store_->pending_plans;
    store_->pending_source_bytes -= source_bytes_;
    active_ = false;
}

axk::app::package_plan_internal::Claim::Claim(std::shared_ptr<Store> store, std::shared_ptr<Record> record)
    : store_(std::move(store)), record_(std::move(record)) {}

axk::app::package_plan_internal::Claim::~Claim() { release(); }

axk::app::package_plan_internal::Claim::Claim(Claim &&other) noexcept
    : store_(std::move(other.store_)), record_(std::move(other.record_)), active_(std::exchange(other.active_, false)) {
}

const std::shared_ptr<axk::app::package_plan_internal::Record> &
axk::app::package_plan_internal::Claim::record() const noexcept {
    return record_;
}

void axk::app::package_plan_internal::Claim::consume() {
    if (!active_)
        return;
    std::lock_guard lock{store_->mutex};
    const auto reservation = normalized_path(record_->output_path);
    if (const auto found = store_->destination_reservations.find(reservation);
        found != store_->destination_reservations.end() && found->second == record_->token) {
        store_->destination_reservations.erase(found);
    }
    store_->plans.erase(record_->token);
    active_ = false;
}

void axk::app::package_plan_internal::Claim::release() {
    if (!active_)
        return;
    std::lock_guard lock{store_->mutex};
    if (const auto found = store_->plans.find(record_->token); found != store_->plans.end())
        found->second->claimed = false;
    active_ = false;
}

axk::app::Result<axk::app::package_plan_internal::Admission>
axk::app::package_plan_internal::admit(const std::shared_ptr<Store> &store, std::uint64_t source_bytes) {
    std::lock_guard lock{store->mutex};
    cleanup(*store, Clock::now());
    if (source_bytes > store->maximum_plan_source_bytes) {
        return std::unexpected(
            plan_error("package_plan_capacity", "package import sources exceed the per-plan byte budget"));
    }
    if (store->plans.size() + store->pending_plans >= store->maximum_plans) {
        return std::unexpected(plan_error("package_plan_capacity", "too many package import plans are active", true));
    }
    const auto retained = retained_source_bytes(*store);
    if (retained > store->maximum_retained_source_bytes ||
        store->pending_source_bytes > store->maximum_retained_source_bytes - retained ||
        source_bytes > store->maximum_retained_source_bytes - retained - store->pending_source_bytes) {
        return std::unexpected(
            plan_error("package_plan_capacity", "package import plan byte budget is exhausted", true));
    }
    ++store->pending_plans;
    store->pending_source_bytes += source_bytes;
    return Admission{store, source_bytes};
}

axk::app::Result<axk::app::package_plan_internal::RetainedSources>
axk::app::package_plan_internal::retain_sources(std::span<const PackageInput> inputs, std::string_view owner_id,
                                                const Sandbox &sandbox, UploadStore &uploads) {
    RetainedSources retained;
    for (const auto &input : inputs) {
        std::uint64_t size{};
        if (const auto *file = std::get_if<FileRef>(&input.reference)) {
            auto opened = sandbox.open_file(*file);
            if (!opened)
                return std::unexpected(opened.error());
            size = opened->reader->size();
        } else {
            const auto &upload = std::get<UploadRef>(input.reference);
            auto snapshot = uploads.inspect(upload, owner_id);
            if (!snapshot)
                return std::unexpected(snapshot.error());
            if (snapshot->state != UploadState::ready || snapshot->kind != UploadKind::package)
                return std::unexpected(plan_error("upload_kind_mismatch", "upload is not a ready portable package"));
            auto lease = uploads.lease(upload, owner_id);
            if (!lease)
                return std::unexpected(lease.error());
            size = snapshot->received_size;
            retained.upload_leases.push_back(std::move(*lease));
        }
        if (size > std::numeric_limits<std::uint64_t>::max() - retained.source_bytes)
            return std::unexpected(plan_error("package_plan_capacity", "package import source byte count overflows"));
        retained.source_bytes += size;
    }
    return retained;
}

axk::app::Result<axk::app::package_plan_internal::Claim>
axk::app::package_plan_internal::claim(const std::shared_ptr<Store> &store, std::string_view token,
                                       std::string_view owner_id) {
    std::lock_guard lock{store->mutex};
    cleanup(*store, Clock::now());
    const auto found = store->plans.find(std::string{token});
    if (found == store->plans.end() || found->second->owner_id != owner_id) {
        return std::unexpected(plan_error("package_plan_not_found", "package import plan is absent or expired"));
    }
    if (found->second->claimed)
        return std::unexpected(plan_error("package_plan_in_use", "package import plan is already being applied"));
    found->second->claimed = true;
    return Claim{store, found->second};
}

axk::app::Result<void> axk::app::package_plan_internal::release(const std::shared_ptr<Store> &store,
                                                                std::string_view token, std::string_view owner_id) {
    std::lock_guard lock{store->mutex};
    cleanup(*store, Clock::now());
    const auto found = store->plans.find(std::string{token});
    if (found == store->plans.end() || found->second->owner_id != owner_id)
        return std::unexpected(plan_error("package_plan_not_found", "package import plan is absent or expired"));
    if (found->second->claimed)
        return std::unexpected(plan_error("package_plan_in_use", "package import plan is already being applied"));
    const auto reservation = normalized_path(found->second->output_path);
    store->destination_reservations.erase(reservation);
    store->plans.erase(found);
    return {};
}

axk::app::Result<std::vector<axk::app::PathAccess>>
axk::app::package_plan_internal::path_accesses(const std::shared_ptr<Store> &store, std::string_view token,
                                               std::string_view owner_id) {
    std::lock_guard lock{store->mutex};
    cleanup(*store, Clock::now());
    const auto found = store->plans.find(std::string{token});
    if (found == store->plans.end() || found->second->owner_id != owner_id || found->second->claimed) {
        return std::unexpected(plan_error("package_plan_not_found", "package import plan is expired or unknown"));
    }
    std::vector<PathAccess> accesses{{found->second->target, PathAccessMode::shared}};
    accesses.reserve(found->second->inputs.size() + 2U);
    for (const auto &input : found->second->inputs) {
        if (const auto *file = std::get_if<FileRef>(&input.reference))
            accesses.push_back({*file, PathAccessMode::shared});
    }
    accesses.push_back({found->second->output, PathAccessMode::exclusive});
    return accesses;
}
