#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

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

// Raw filesystem edits do not repair or require valid sampler relationships.
// Callers must review that consequence before invoking a mutation.
// validate_inputs runs inside journal commit validation; failure rolls back.
[[nodiscard]] Result<ImageSessionSummary>
apply_filesystem_edits(ImageSessionManager &images, AlterationJournalStore &journals, std::string_view image_id,
                       std::string_view owner_id, std::uint64_t expected_revision, PartitionIndex partition,
                       std::span<const FilesystemEdit> edits, const CancellationToken &cancellation = {},
                       ProgressSink *progress = nullptr, const std::function<Result<void>()> &validate_inputs = {});

} // namespace axk::app
