#include "prepared_session_commit.hpp"

#include <chrono>
#include <optional>
#include <utility>

#include "axklib/application/volume_capacity.hpp"

namespace axk::app {

Result<ImageSessionSummary>
commit_prepared_session(ImageSessionManager &images, AlterationJournalStore &journals, ImageSessionMutation &mutation,
                        std::span<const AlterationJournalPatch> patches, const VolumeCapacityAdmission &capacity,
                        const VolumeCapacityPolicy &policy, const OperationContext &context,
                        const std::function<Result<void>()> &verify_inputs,
                        const std::function<void(const SessionCommitObservation &)> &before_publication) {
    struct Guard {
        ImageSessionManager &images;
        ImageSessionMutation &mutation;
        std::string_view owner;
        bool finished{};
        bool invalidate{};
        ~Guard() {
            if (!finished)
                images.abort_mutation(mutation.image_id, owner, mutation.revision, invalidate);
        }
    } guard{images, mutation, context.owner_id};
    if (context.progress)
        context.progress->report({ProgressPhase::publishing, 0U, 1U, "Committing image changes", std::nullopt});
    std::optional<PreparedImageSessionCommit> prepared_commit;
    SessionCommitObservation observation;
    const auto validate = [&]() -> Result<void> {
        const auto started = std::chrono::steady_clock::now();
        auto checked =
            images.prepare_mutation_commit(mutation.image_id, context.owner_id, mutation.revision, CancellationToken{});
        observation.post_write_reopen_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        if (!checked)
            return std::unexpected(checked.error());
        prepared_commit.emplace(std::move(*checked));
        return {};
    };
    const auto admit = [&](std::shared_ptr<const RandomAccessReader> frozen) -> Result<void> {
        if (verify_inputs) {
            if (auto verified = verify_inputs(); !verified)
                return verified;
        }
        auto current = inspect_frozen_capacity(std::move(frozen), capacity, policy, context.cancellation);
        if (!current)
            return std::unexpected(current.error());
        if (auto admitted = enforce_volume_capacity_admission(*current, policy); !admitted)
            return std::unexpected(Error{"volume_capacity_exceeded", admitted.error().message});
        return {};
    };
    bool rollback_verified{};
    guard.invalidate = true;
    if (auto applied = images.apply_mutation(
            journals, mutation, patches, context.cancellation, validate, [&] { rollback_verified = true; }, admit);
        !applied) {
        guard.invalidate = !journals.storage_ready() || applied.error().code == "device_recovery_required";
        if (rollback_verified && !guard.invalidate) {
            if (auto restored =
                    images.refresh_rolled_back_mutation(mutation.image_id, context.owner_id, mutation.revision);
                !restored) {
                guard.invalidate = true;
                return std::unexpected(restored.error());
            }
        }
        return std::unexpected(applied.error());
    }
    if (!prepared_commit)
        return std::unexpected(Error{"image_operation_failed", "Image mutation validation did not complete"});
    observation.cleanup_pending = !journals.storage_ready();
    if (before_publication)
        before_publication(observation);
    mutation.target.reset();
    auto summary = images.finalize_mutation_commit(std::move(*prepared_commit));
    guard.finished = true;
    if (context.progress)
        context.progress->report({ProgressPhase::publishing, 1U, 1U, "Image changes committed", std::nullopt});
    return summary;
}

} // namespace axk::app
