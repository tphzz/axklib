#include "axklib/application/device_xml.hpp"

#include <limits>
#include <utility>

#include "axklib/application/device_sessions.hpp"
#ifdef AXK_BUILD_DEVICES
#include "rawdisk/scsi2sd/xml.hpp"
#include "rawdisk/volume.hpp"
#endif

namespace axk::app {
Result<std::vector<DeviceXmlTarget>> inspect_scsi2sd_xml(std::string_view xml) {
#ifdef AXK_BUILD_DEVICES
    const auto config = rawdisk::scsi2sd::xml::parse(xml);
    if (!config)
        return std::unexpected(Error{"device_layout_invalid",
                                     "Invalid SCSI2SD XML: check its structure, enabled "
                                     "target geometry and unique SCSI IDs (256 KiB maximum; no DTDs)."});
    const auto volumes = rawdisk::scsi2sd::xml::to_volumes(*config, std::numeric_limits<std::uint64_t>::max() / 512U,
                                                           rawdisk::scsi2sd::xml::Board::v5);
    std::vector<DeviceXmlTarget> result;
    for (const auto &target : config->targets) {
        const auto id = std::to_string(target.scsi_id);
        const auto *volume = volumes.find(id);
        const auto size = static_cast<std::uint64_t>(target.scsi_sectors) * target.bytes_per_sector;
        DeviceXmlTarget value{id,
                              "SCSI ID " + id,
                              size,
                              target.bytes_per_sector,
                              target.enabled,
                              target.enabled && volume && volume->usable,
                              !volume || !rawdisk::check_writable(volumes, id) || target.bytes_per_sector != 512U,
                              {}};
        if (target.enabled && volume) {
            value.label = volume->label;
            for (const auto &notice : volume->notices)
                value.notices.push_back(notice.text);
        }
        if (!target.enabled)
            value.notices.push_back("Disabled in XML.");
        result.push_back(std::move(value));
    }
    return result;
#else
    (void)xml;
    return std::unexpected(Error{"devices_unavailable", "this build does not include device access"});
#endif
}
} // namespace axk::app
