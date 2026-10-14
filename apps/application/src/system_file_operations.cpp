#include "axklib/application/system_file_operations.hpp"

#include <utility>

#include "axklib/filesystem_transaction.hpp"
#include "filesystem_session_mutation.hpp"
#include "write_operations_internal.hpp"

namespace axk::app {

Result<ImageSessionSummary> apply_system_file(ImageSessionManager &images, AlterationJournalStore &journals,
                                              const ImageDocumentRef &target, std::string_view owner_id,
                                              std::uint64_t expected_revision, PartitionIndex partition,
                                              const SystemFilePatch &patch, ASeriesModel model,
                                              const CancellationToken &cancellation, ProgressSink *progress) {
    const auto document = images.document(target, owner_id);
    if (!document)
        return std::unexpected(document.error());
    if (document->format_id != "yamaha.a-series" || document->source_member_ids.size() != 1U)
        return std::unexpected(Error{"image_mutation_unsupported", "System File editing requires one A-series source"});
    const ImageSourceTarget source{target.image_id, document->source_member_ids.front()};
    const auto prepare = [&](const ImageSessionMutation &mutation) -> Result<axk::detail::PreparedFilesystemEdits> {
        if (mutation.media_kind != MediaKind::sfs)
            return std::unexpected(Error{"image_mutation_unsupported", "System File editing requires SFS media"});
        auto result = axk::detail::prepare_sfs_system_file(mutation.target, partition, patch, model, cancellation);
        if (!result)
            return std::unexpected(write_operations_internal::core_error(result.error()));
        return std::move(*result);
    };
    return detail::apply_session_filesystem_mutation(images, journals, source, owner_id, expected_revision, partition,
                                                     prepare, {}, cancellation, progress);
}

} // namespace axk::app
