#pragma once

#include "rawdisk/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <system_error>

namespace rawdisk::detail {

struct Win32UsbIdentity {
    std::string carrier_id;
    std::string connection_id;
};

template <class Api> Result<Win32UsbIdentity> win32_node_identity(Api &api, std::uint32_t instance) {
    const auto disk_id = api.device_id(instance);
    if (!disk_id)
        return std::unexpected(disk_id.error());
    if (disk_id->empty())
        return std::unexpected(make_error_code(Errc::device_identity_unavailable));
    const auto disk_arrival = api.last_arrival(instance);
    if (!disk_arrival)
        return std::unexpected(disk_arrival.error());
    if (*disk_arrival == 0)
        return std::unexpected(make_error_code(Errc::device_identity_unavailable));
    for (std::size_t depth = 0; depth < 64; ++depth) {
        auto name = api.device_id(instance);
        if (!name)
            return std::unexpected(name.error());
        if (name->empty())
            return std::unexpected(make_error_code(Errc::device_identity_unavailable));
        if (name->starts_with("USB\\VID_") && name->find("&MI_") == std::string::npos) {
            const auto usb_arrival = api.last_arrival(instance);
            if (!usb_arrival)
                return std::unexpected(usb_arrival.error());
            if (*usb_arrival == 0)
                return std::unexpected(make_error_code(Errc::device_identity_unavailable));
            // DEVINST values are opaque handles, not identities shared with the UAC helper.
            return Win32UsbIdentity{*name, std::format("{}:{}:{}:{}:{}:{}", name->size(), *name, disk_id->size(),
                                                       *disk_id, *usb_arrival, *disk_arrival)};
        }
        auto parent = api.parent(instance);
        if (!parent)
            return std::unexpected(parent.error());
        instance = *parent;
    }
    return std::unexpected(make_error_code(Errc::device_identity_unavailable));
}

inline std::error_code win32_identity_mismatch(const DeviceInfo &actual, const DeviceInfo &expected) noexcept {
    if (actual.path != expected.path)
        return make_error_code(Errc::device_changed);
    if (actual.carrier_id.empty() || actual.connection_id.empty() || expected.carrier_id.empty() ||
        expected.connection_id.empty())
        return make_error_code(Errc::device_identity_unavailable);
    if (actual.carrier_id != expected.carrier_id || actual.connection_id != expected.connection_id)
        return make_error_code(Errc::device_connection_changed);
    if (actual.model != expected.model || actual.serial != expected.serial)
        return make_error_code(Errc::device_descriptor_changed);
    if (actual.size_bytes != expected.size_bytes || actual.sector_size != expected.sector_size)
        return make_error_code(Errc::device_geometry_changed);
    return {};
}

} // namespace rawdisk::detail
