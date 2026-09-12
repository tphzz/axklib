#include "axklib/application/filesystem_edit_operations.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/filesystem_transaction.hpp"
#include "content_digest.hpp"
#include "write_operations_internal.hpp"

namespace axk::app {
namespace {
struct MutationGuard {
    ImageSessionManager &images;
    const ImageSessionMutation &mutation;
    std::string_view owner;
    bool finished{};
    ~MutationGuard() {
        if (!finished)
            images.abort_mutation(mutation.image_id, owner, mutation.revision);
    }
};
} // namespace

Result<ImageSessionSummary> apply_filesystem_edits(ImageSessionManager &images, AlterationJournalStore &journals,
                                                   std::string_view image_id, std::string_view owner_id,
                                                   std::uint64_t expected_revision, PartitionIndex partition,
                                                   std::span<const FilesystemEdit> edits,
                                                   const CancellationToken &cancellation, ProgressSink *progress,
                                                   const FilesystemInputVerification &input_verification) {
    using write_operations_internal::core_error;
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected(core_error(checked.error()));
    if (!input_verification.reviewed_readers.empty() && !input_verification.verify)
        return std::unexpected(Error{"invalid_request", "Reviewed inputs require commit verification"});
    auto mutation = images.begin_filesystem_mutation(image_id, owner_id, expected_revision, partition);
    if (!mutation)
        return std::unexpected(mutation.error());
    MutationGuard guard{images, *mutation, owner_id};
    std::map<std::shared_ptr<const RandomAccessReader>, std::string> inputs;
    for (const auto &edit : edits) {
        const auto *put = std::get_if<PutFilesystemFile>(&edit);
        if (!put || !put->contents || inputs.contains(put->contents))
            continue;
        if (put->contents->size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(Error{"filesystem_input_too_large", "file input exceeds the filesystem size field"});
        if (std::ranges::find(input_verification.reviewed_readers, put->contents) !=
            input_verification.reviewed_readers.end())
            continue;
        auto hash = detail::reader_sha256(*put->contents, cancellation);
        if (!hash)
            return std::unexpected(hash.error());
        inputs.emplace(put->contents, std::move(*hash));
    }
    if (progress)
        progress->report({ProgressPhase::allocating, 0U, std::nullopt, "Planning filesystem changes", std::nullopt});
    const auto prepared = mutation->media_kind == MediaKind::sfs
                              ? axk::detail::prepare_sfs_file_edits(mutation->target, partition, edits, cancellation)
                              : axk::detail::prepare_fat_file_edits(mutation->target, partition, edits, cancellation);
    if (!prepared)
        return std::unexpected(core_error(prepared.error()));
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
        if (input_verification.verify) {
            if (auto result = input_verification.verify(); !result)
                return result;
        }
        for (const auto &[reader, expected] : inputs) {
            const auto hash = detail::reader_sha256(*reader, cancellation);
            if (!hash)
                return std::unexpected(hash.error());
            if (*hash != expected)
                return std::unexpected(Error{"filesystem_input_changed", "file input changed during import", {}, true});
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
    if (auto applied = journals.apply(mutation->target, prepared->image_size_bytes, patches, cancellation, validate,
                                      [&] { rollback_verified = true; });
        !applied) {
        refreshed.reset();
        if (rollback_verified && journals.storage_ready()) {
            if (auto bound = mutation->target->verify_bound(); !bound)
                return std::unexpected(bound.error());
            if (auto restored = images.refresh_rolled_back_mutation(image_id, owner_id, expected_revision); !restored)
                return std::unexpected(restored.error());
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
} // namespace axk::app
