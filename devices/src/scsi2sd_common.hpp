#pragma once

#include "rawdisk/scsi2sd/common.hpp"
#include "rawdisk/volume.hpp"

#include <span>
#include <string>
#include <vector>

namespace rawdisk::scsi2sd::detail {

// Enabled targets -> volumes with all findings. `v6_board`: the board keeps its configuration in
// the last 2 card sectors (and has the misplaced-save bug), so those findings apply.
VolumeSet build_volumes(std::string scheme, std::span<const Target> targets, std::uint64_t card_sectors, bool v6_board,
                        std::vector<Range> protected_ranges);

} // namespace rawdisk::scsi2sd::detail
