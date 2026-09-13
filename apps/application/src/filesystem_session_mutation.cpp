#include "filesystem_session_mutation.hpp"

#include <optional>
#include <utility>
#include <vector>

#include "write_operations_internal.hpp"

namespace axk::app::detail {
namespace {
struct MutationGuard {
    ImageSessionManager &images;
    const ImageSessionMutation &mutation;
    std::string_view owner;
    bool finished{};
    bool invalidate_session{};
    ~MutationGuard() {
        if (!finished)
            images.abort_mutation(mutation.image_id, owner, mutation.revision, invalidate_session);
    }
};
} // namespace

Result<ImageSessionSummary> apply_session_filesystem_mutation(
    ImageSessionManager &images, AlterationJournalStore &journals, std::string_view image_id, std::string_view owner_id,
    std::uint64_t expected_revision, PartitionIndex partition, const SessionFilesystemPlanner &prepare,
    const std::function<Result<void>()> &verify_inputs, const CancellationToken &cancellation, ProgressSink *progress) {
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected(write_operations_internal::core_error(checked.error()));
    auto mutation = images.begin_filesystem_mutation(image_id, owner_id, expected_revision, partition);
    if (!mutation)
        return std::unexpected(mutation.error());
    MutationGuard guard{images, *mutation, owner_id};
    if (progress)
        progress->report({ProgressPhase::allocating, 0U, std::nullopt, "Planning filesystem changes", std::nullopt});
    const auto prepared = prepare(*mutation);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (auto unchanged = mutation->target->verify_unchanged(); !unchanged)
        return std::unexpected(unchanged.error());
    std::vector<AlterationJournalPatch> patches;
    patches.reserve(prepared->patches.size());
    for (const auto &patch : prepared->patches)
        patches.push_back({patch.offset,
                           {mutation->target, patch.offset, patch.size},
                           {patch.source, patch.source_offset, patch.size}});
    std::optional<PreparedImageSessionCommit> refreshed;
    const auto validate = [&]() -> Result<void> {
        if (verify_inputs) {
            if (auto result = verify_inputs(); !result)
                return result;
        }
        if (progress)
            progress->report({ProgressPhase::validating, 0U, 1U, "Refreshing image metadata", std::nullopt});
        auto result = images.prepare_mutation_commit(image_id, owner_id, expected_revision, cancellation);
        if (!result)
            return std::unexpected(result.error());
        refreshed.emplace(std::move(*result));
        return {};
    };
    if (progress)
        progress->report({ProgressPhase::publishing, 0U, 1U, "Committing filesystem changes", std::nullopt});
    bool rollback_verified{};
    guard.invalidate_session = true;
    if (auto applied = journals.apply(mutation->target, prepared->image_size_bytes, patches, cancellation, validate,
                                      [&] { rollback_verified = true; });
        !applied) {
        refreshed.reset();
        guard.invalidate_session = !journals.storage_ready() || rollback_verified;
        if (rollback_verified && journals.storage_ready()) {
            if (auto bound = mutation->target->verify_bound(); !bound)
                return std::unexpected(bound.error());
            if (auto restored = images.refresh_rolled_back_mutation(image_id, owner_id, expected_revision); !restored)
                return std::unexpected(restored.error());
            guard.invalidate_session = false;
        }
        return std::unexpected(applied.error());
    }
    if (!refreshed)
        return std::unexpected(Error{"filesystem_edit_failed", "filesystem commit validation did not finish"});
    mutation->target.reset();
    auto summary = images.finalize_mutation_commit(std::move(*refreshed));
    guard.finished = true;
    if (progress)
        progress->report({ProgressPhase::publishing, 1U, 1U, "Filesystem changes committed", std::nullopt});
    return summary;
}

} // namespace axk::app::detail
