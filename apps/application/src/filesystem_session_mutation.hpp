#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/filesystem_transaction.hpp"

namespace axk::app::detail {

using SessionFilesystemPlanner =
    std::function<Result<axk::detail::PreparedFilesystemEdits>(const ImageSessionMutation &)>;

// Planning and input verification run while the session owns exclusive mutation
// access. Only the changed ranges are frozen in the rollback journal.
[[nodiscard]] Result<ImageSessionSummary> apply_session_filesystem_mutation(
    ImageSessionManager &images, AlterationJournalStore &journals, std::string_view image_id, std::string_view owner_id,
    std::uint64_t expected_revision, PartitionIndex partition, const SessionFilesystemPlanner &prepare,
    const std::function<Result<void>()> &verify_inputs, const CancellationToken &cancellation, ProgressSink *progress);

} // namespace axk::app::detail
