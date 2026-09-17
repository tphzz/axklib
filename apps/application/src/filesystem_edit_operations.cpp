#include "axklib/application/filesystem_edit_operations.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include "axklib/filesystem_transaction.hpp"
#include "content_digest.hpp"
#include "filesystem_session_mutation.hpp"
#include "write_operations_internal.hpp"

namespace axk::app {
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
    std::map<std::shared_ptr<const RandomAccessReader>, std::string> inputs;
    const auto prepare = [&](const ImageSessionMutation &mutation) -> Result<axk::detail::PreparedFilesystemEdits> {
        for (const auto &edit : edits) {
            const auto *put = std::get_if<PutFilesystemFile>(&edit);
            if (!put || !put->contents || inputs.contains(put->contents))
                continue;
            if (put->contents->size() > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected(
                    Error{"filesystem_input_too_large", "file input exceeds the filesystem size field"});
            if (std::ranges::find(input_verification.reviewed_readers, put->contents) !=
                input_verification.reviewed_readers.end())
                continue;
            auto hash = detail::reader_sha256(*put->contents, cancellation);
            if (!hash)
                return std::unexpected(hash.error());
            inputs.emplace(put->contents, std::move(*hash));
        }
        auto prepared = mutation.media_kind == MediaKind::sfs
                            ? axk::detail::prepare_sfs_file_edits(mutation.target, partition, edits, cancellation)
                            : axk::detail::prepare_fat_file_edits(mutation.target, partition, edits, cancellation);
        if (!prepared)
            return std::unexpected(core_error(prepared.error()));
        return std::move(*prepared);
    };
    const auto verify_inputs = [&]() -> Result<void> {
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
        return {};
    };
    return detail::apply_session_filesystem_mutation(images, journals, image_id, owner_id, expected_revision, partition,
                                                     prepare, verify_inputs, cancellation, progress);
}
} // namespace axk::app
