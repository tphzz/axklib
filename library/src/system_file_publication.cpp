#include "axklib/system_file_write.hpp"

#include <memory>
#include <utility>

#include "axklib/filesystem_transaction.hpp"
#include "sfs_files_internal.hpp"

namespace axk {

Result<PublicationOutcome> write_system_file(const std::filesystem::path &source,
                                             const std::filesystem::path &destination, PartitionIndex partition,
                                             const SystemFilePatch &patch, ASeriesModel model,
                                             const CancellationToken &cancellation, ProgressSink *progress) {
    return sfs_files::publish(
        source, destination, partition, {},
        [&](std::shared_ptr<const RandomAccessReader> reader) {
            return detail::prepare_sfs_system_file(std::move(reader), partition, patch, model, cancellation);
        },
        cancellation, progress);
}

} // namespace axk
