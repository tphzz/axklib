#include "image_sessions_internal.hpp"

axk::app::Result<void> axk::app::ImageSessionManager::apply_mutation(
    AlterationJournalStore &journals, const ImageSessionMutation &mutation,
    std::span<const AlterationJournalPatch> patches, const CancellationToken &cancellation,
    const std::function<Result<void>()> &validate, const std::function<void()> &on_rollback_verified,
    const std::function<Result<void>(std::shared_ptr<const RandomAccessReader>)> &admit_frozen) {
    if (mutation.source.kind == ImageSourceKind::device) {
        if (!implementation_->devices)
            return std::unexpected(Error{"devices_unavailable", "device access is disabled"});
        return implementation_->devices->apply(mutation.target, patches, cancellation, validate, on_rollback_verified,
                                               admit_frozen);
    }
    return journals.apply(mutation.target, mutation.target->size(), patches, cancellation, validate,
                          on_rollback_verified, admit_frozen);
}
