#pragma once

#include "device_sessions_internal.hpp"
#include "device_text.hpp"

#include <array>
#include <utility>

namespace axk::app::device_detail {
inline Json identity_json(const DeviceIdentityDetails &identity) {
    return {{"devicePath", identity.device_path},
            {"usbManufacturer", device_text(identity.usb_manufacturer)},
            {"usbProduct", device_text(identity.usb_product)},
            {"usbSerial", device_text(identity.usb_serial)},
            {"usbVendorId", device_text(identity.usb_vendor_id)},
            {"usbProductId", device_text(identity.usb_product_id)}};
}
inline Json candidate_json(const DeviceCandidate &candidate) {
    return {{"id", candidate.id},
            {"model", device_text(candidate.model)},
            {"serial", device_text(candidate.serial)},
            {"sizeBytes", candidate.size_bytes},
            {"sectorSize", candidate.sector_size},
            {"identity", identity_json(candidate.identity)}};
}
inline Json target_json(const DeviceTargetDescription &target, const DeviceCandidate &candidate) {
    Json scsi = nullptr;
    if (target.scsi) {
        static constexpr std::array types{"FIXED", "REMOVABLE", "OPTICAL", "FLOPPY", "MAGNETO_OPTICAL", "SEQUENTIAL"};
        scsi = {{"id", target.scsi->id},
                {"deviceType", types.at(target.scsi->device_type)},
                {"vendor", device_text(target.scsi->vendor)},
                {"product", device_text(target.scsi->product)},
                {"revision", device_text(target.scsi->revision)},
                {"serial", device_text(target.scsi->serial)}};
    }
    return {{"id", target.id},
            {"label", device_text(target.label)},
            {"offsetBytes", target.offset},
            {"sizeBytes", target.size_bytes},
            {"sectorSize", target.sector_size},
            {"readOnly", target.read_only},
            {"notices", target.notices},
            {"scsi", std::move(scsi)},
            {"recoveryConfirmationText",
             candidate.identity.device_path.empty() ? "" : device_confirmation(candidate, target, "RESTORE")}};
}
inline Json guard_json(const Guards &guards) {
    auto result = Json::array();
    for (const auto &[offset, hash] : guards)
        result.push_back({{"offset", offset}, {"sha256", hash}});
    return {{"sectors", std::move(result)}};
}
} // namespace axk::app::device_detail
