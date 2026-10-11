// SCSI2SD layouts from the XML file exported by scsi2sd-util (V5 and V6). V5 boards keep their
// configuration in the board's flash, so for V5 cards this is the only source of the layout.
#pragma once

#include "rawdisk/block_device.hpp"
#include "rawdisk/scsi2sd/common.hpp"
#include "rawdisk/volume.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rawdisk::scsi2sd::xml {

struct Config {
    std::vector<std::pair<std::string, std::string>> board; // BoardConfig / S2S_BoardCfg settings
    std::vector<Target> targets;                            // disabled targets included
};

// Pure validation: at most 256 KiB, bounded depth, no DTDs or external entities.
// Malformed input, duplicate enabled IDs or incomplete enabled geometry is rejected.
// The XML dialect does not establish the connected board version or write safety.
Result<Config> parse(std::string_view xml) noexcept;
Result<Config> load(const std::filesystem::path &path) noexcept;

enum class Board {
    v6, // configuration in the last 2 card sectors: they are protected, V6 findings apply
    v5, // configuration in the board's flash: nothing on the card is reserved
};

VolumeSet to_volumes(const Config &config, std::uint64_t card_sectors, Board board = Board::v6);

} // namespace rawdisk::scsi2sd::xml
