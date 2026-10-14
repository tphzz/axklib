#pragma once

#include <functional>
#include <span>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/volume_capacity.hpp"

namespace axk::app {

struct SessionCommitObservation {
    std::uint64_t post_write_reopen_us{};
    bool cleanup_pending{};
};

// Takes responsibility for abort/rollback and revision publication after admission.
[[nodiscard]] Result<ImageSessionSummary>
commit_prepared_session(ImageSessionManager &images, AlterationJournalStore &journals, ImageSessionMutation &mutation,
                        std::span<const AlterationJournalPatch> patches, const VolumeCapacityAdmission &capacity,
                        const VolumeCapacityPolicy &policy, const OperationContext &context,
                        const std::function<Result<void>()> &verify_inputs = {},
                        const std::function<void(const SessionCommitObservation &)> &before_publication = {});

} // namespace axk::app
