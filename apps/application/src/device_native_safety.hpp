#pragma once

#include <algorithm>

#include "device_sessions_internal.hpp"
#include "rawdisk/inspection.hpp"

namespace axk::app::device_detail {

inline DeviceIdentityDetails identity_details(const rawdisk::DeviceInfo &info,
                                              const rawdisk::DevicePresentation &value) {
    return {info.path,        value.usb_manufacturer, value.usb_product,
            value.usb_serial, value.usb_vendor_id,    value.usb_product_id};
}

inline Result<DeviceWriteSafety> native_write_safety(const rawdisk::DeviceInfo &info,
                                                     rawdisk::BlockDevice *retained_claim = nullptr) {
    auto inspected = rawdisk::inspect_usb_device(info, retained_claim);
    if (!inspected)
        return std::unexpected(Error{"device_safety_unavailable", inspected.error().message()});
    DeviceWriteSafety result;
    result.identity = identity_details(info, inspected->presentation);
    Json membership = Json::array();
    std::vector<std::string> stable;
    for (const auto &member : inspected->members) {
        const Json identity{{"model", member.model},
                            {"serial", member.serial},
                            {"size", member.size_bytes},
                            {"sector", member.sector_size}};
        stable.push_back(identity.dump());
        membership.push_back({{"identity", identity}, {"path", member.path}, {"connection", member.connection_id}});
    }
    std::ranges::sort(stable);
    const auto snapshot = membership.dump();
    const auto retained = Json{stable}.dump();
    auto bound = digest(std::as_bytes(std::span{snapshot.data(), snapshot.size()}));
    auto recovery = digest(std::as_bytes(std::span{retained.data(), retained.size()}));
    if (!bound || !recovery)
        return std::unexpected(!bound ? bound.error() : recovery.error());
    result.membership = *bound;
    result.recovery_membership = *recovery;
    result.complete = inspected->complete;
    result.non_dismounting_claim_supported = inspected->non_dismounting_claim_supported;
    result.notices = inspected->notices;
    for (const auto &volume : inspected->mounted)
        result.mounted.push_back({volume.device, volume.mount_point, volume.filesystem, volume.label});
    return result;
}
} // namespace axk::app::device_detail
