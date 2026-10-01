#include "handlers.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>

#include "axklib/application/volume_capacity.hpp"
#include "axklib/sfs.hpp"
#include "axklib/volume_capacity.hpp"
#include "support.hpp"

namespace axk::cli::commands {
int run_volume_capacity(const VolumeCapacityRequest &request) {
    const auto image = open_image(request.source);
    if (!image)
        return report_failure(image.error());
    const auto partition =
        std::ranges::find(image->partitions(), PartitionIndex{request.partition_index}, &Partition::index);
    if (partition == image->partitions().end()) {
        std::cerr << "error: partition does not exist\n";
        return exit_code(ExitStatus::invalid_request);
    }
    const auto root_id = locate_partition_root_record(*partition);
    if (!root_id)
        return report_failure(root_id.error());
    const auto root = std::ranges::find(partition->records, *root_id, &IndexRecord::sfs_id);
    std::optional<SfsId> volume;
    for (const auto &entry : root->directory_entries) {
        if (entry.name != request.volume_name || !entry.target_link_id || entry.state != DirectoryEntryState::live)
            continue;
        for (const auto &record : partition->records) {
            if (record.directory_id != entry.target_link_id || record.parent_directory_id != root->directory_id)
                continue;
            if (volume) {
                std::cerr << "error: volume name is ambiguous\n";
                return exit_code(ExitStatus::invalid_request);
            }
            volume = record.sfs_id;
        }
    }
    if (!volume) {
        std::cerr << "error: volume does not exist\n";
        return exit_code(ExitStatus::invalid_request);
    }
    const auto report = inspect_volume_capacity(*image, partition->index, *volume);
    if (!report)
        return report_failure(report.error());
    std::cout << app::volume_capacity_json(*report).dump(request.pretty ? 2 : -1) << '\n';
    return exit_code(ExitStatus::success);
}
} // namespace axk::cli::commands
