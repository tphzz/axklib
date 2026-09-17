#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/filesystem_edit.hpp"

namespace axk::app {

[[nodiscard]] Result<void> bind_filesystem_import_inspection(OperationRegistry &registry, ImageSessionManager &images);

[[nodiscard]] Result<void> bind_filesystem_edit_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                           UploadStore &uploads, ImageSessionManager &images,
                                                           AlterationJournalStore &journals);

struct FilesystemInputVerification {
    // Already verified against reviewed content before planning. The callback
    // owns their backing-source checks during commit, including native identity.
    std::vector<std::shared_ptr<const RandomAccessReader>> reviewed_readers;
    std::function<Result<void>()> verify;
};

// Raw filesystem edits do not repair or require valid sampler relationships.
// Callers must review that consequence before invoking a mutation.
// Unlisted readers receive before/after digest checks here. The supplied
// verifier owns the listed readers' checks; failure rolls back.
[[nodiscard]] Result<ImageSessionSummary>
apply_filesystem_edits(ImageSessionManager &images, AlterationJournalStore &journals, std::string_view image_id,
                       std::string_view owner_id, std::uint64_t expected_revision, PartitionIndex partition,
                       std::span<const FilesystemEdit> edits, const CancellationToken &cancellation = {},
                       ProgressSink *progress = nullptr, const FilesystemInputVerification &input_verification = {});

} // namespace axk::app
