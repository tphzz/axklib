#pragma once

#include <cstdint>
#include <string_view>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/io.hpp"
#include "axklib/sampler_model.hpp"
#include "axklib/system_file_parameters.hpp"
#include "axklib/types.hpp"

namespace axk::app {

// Edits the retained partition System file under revision and exclusive-path
// checks. The journal freezes only changed ranges; successful publication
// refreshes session metadata. Missing System files are never created.
// Global preferences, recording configuration and effects share one validation and commit/rollback boundary.
[[nodiscard]] Result<ImageSessionSummary>
apply_system_file(ImageSessionManager &images, AlterationJournalStore &journals, std::string_view image_id,
                  std::string_view owner_id, std::uint64_t expected_revision, PartitionIndex partition,
                  const SystemFilePatch &patch, ASeriesModel model, const CancellationToken &cancellation = {},
                  ProgressSink *progress = nullptr);

} // namespace axk::app
