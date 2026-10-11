// Built-in building blocks. Combine them into your own modes with make_mode().
#pragma once

#include "rawdisk/open/flow.hpp"
#include "rawdisk/scsi2sd/xml.hpp"

#include <optional>

namespace rawdisk::open {

// Step "device", field "device": a USB disk from the context's DeviceSource. Option ids encode
// the identity (model, serial, size, sector size), never the drive number.
std::unique_ptr<Block> select_device(std::optional<std::uint64_t> max_bytes = std::nullopt);

// Step "image", fields "path" (file) and "sector_size" (choice, default 512; omitted when fixed).
std::unique_ptr<Block> select_image(std::optional<std::uint32_t> fixed_sector_size = std::nullopt);

// Step "scsi2sd": SCSI2SD V6 configuration from the card, found automatically (no fields). If the
// card has none, field "use_default" (confirm) must be "yes" to use the firmware default.
std::unique_ptr<Block> scsi2sd_v6();

// Step "xml": layout from a scsi2sd-util XML file, field "file"; plus field "board" (v6/v5,
// default v6) unless the board is fixed by the mode.
std::unique_ptr<Block> scsi2sd_xml(std::optional<scsi2sd::xml::Board> fixed_board = std::nullopt);

// Step "volume", field "volume": one volume of the preceding layout. For ReadWrite, volumes that
// may not be written are disabled with a reason, and field "acknowledge" (confirm) is required
// for volumes with danger notices.
std::unique_ptr<Block> select_volume(std::optional<std::uint64_t> max_bytes = std::nullopt);

// Identity key of a device, as used in option ids and locators.
std::string device_key(const DeviceInfo &info);

} // namespace rawdisk::open
